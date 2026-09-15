#include "asset_storage.h"
#include "audio/assistant_model_bundle.h"
#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace asset_storage {
namespace {
constexpr char kJournalMagic[8] = {'A', 'U', 'D', 'A', 'J', '1', 0, 0};
constexpr char kManifestMagic[8] = {'A', 'U', 'D', 'A', 'S', '1', 0, 0};
constexpr size_t kManifestHeader = 12;
constexpr size_t kJournalPayload = 140;
constexpr uint32_t kMaximumSequence = 2147483647;
void Put32(unsigned char* bytes, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[i] = static_cast<unsigned char>(value >> (i * 8));
}
uint32_t Get32(const unsigned char* bytes) { return audream::ReadBundleU32(bytes); }
bool Empty(const Digest& digest) { return std::all_of(digest.begin(), digest.end(), [](auto byte) { return byte == 0; }); }
bool ValidImage(const Image& image) {
    if (!image.slot) return !image.sequence && Empty(image.digest);
    return image.slot <= 2 && image.sequence && image.sequence <= kMaximumSequence && !Empty(image.digest);
}
bool ValidJournal(const Journal& journal) {
    auto layout = Layout::Create(journal.partition_bytes, journal.factory_bytes);
    return layout && layout->BankBytes() == journal.bank_bytes && !Empty(journal.factory_digest) &&
        journal.high_water <= kMaximumSequence && ValidImage(journal.confirmed) && ValidImage(journal.pending) &&
        journal.confirmed.sequence <= journal.high_water &&
        (!journal.trial_started || journal.pending.slot) &&
        (!journal.pending.slot || (journal.pending.slot != journal.confirmed.slot &&
            journal.pending.sequence == journal.high_water && journal.pending.sequence > journal.confirmed.sequence));
}
void PutImage(unsigned char* bytes, const Image& image) {
    Put32(bytes, image.slot); Put32(bytes + 4, image.sequence);
    std::copy(image.digest.begin(), image.digest.end(), bytes + 8);
}
Image GetImage(const unsigned char* bytes) {
    Image image; image.slot = Get32(bytes); image.sequence = Get32(bytes + 4);
    std::copy_n(bytes + 8, 32, image.digest.begin());
    return image;
}
}  // namespace

std::optional<Layout> Layout::Create(uint32_t partition_bytes, uint32_t factory_bytes) {
    if (partition_bytes > asset_release::kMaximumBundleBytes || partition_bytes % kMapAlignment ||
        factory_bytes < 12 || factory_bytes > partition_bytes) return std::nullopt;
    Layout layout;
    layout.partition_bytes_ = partition_bytes;
    layout.factory_bytes_ = factory_bytes;
    layout.first_bank_ = ((factory_bytes + kMapAlignment - 1) / kMapAlignment) * kMapAlignment;
    layout.bank_bytes_ = ((partition_bytes - layout.first_bank_) / (2 * kMapAlignment)) * kMapAlignment;
    if (layout.bank_bytes_ < kMapAlignment) return std::nullopt;
    return layout;
}
uint32_t Layout::Offset(unsigned slot) const {
    return slot >= 1 && slot <= 2 ? first_bank_ + (slot - 1) * bank_bytes_ : 0;
}
uint32_t Layout::ManifestOffset(unsigned slot) const {
    return slot >= 1 && slot <= 2 ? Offset(slot) + Capacity() : 0;
}

std::optional<JournalBytes> EncodeJournal(const Journal& journal) {
    if (!ValidJournal(journal) || sodium_init() < 0) return std::nullopt;
    JournalBytes bytes{};
    std::memcpy(bytes.data(), kJournalMagic, 8);
    Put32(bytes.data() + 8, journal.partition_bytes);
    Put32(bytes.data() + 12, journal.factory_bytes);
    Put32(bytes.data() + 16, journal.bank_bytes);
    Put32(bytes.data() + 20, journal.high_water);
    Put32(bytes.data() + 24, journal.trial_started ? 1 : 0);
    std::copy(journal.factory_digest.begin(), journal.factory_digest.end(), bytes.begin() + 28);
    PutImage(bytes.data() + 60, journal.confirmed);
    PutImage(bytes.data() + 100, journal.pending);
    if (crypto_hash_sha256(bytes.data() + kJournalPayload, bytes.data(), kJournalPayload) != 0) return std::nullopt;
    return bytes;
}
std::optional<Journal> DecodeJournal(const JournalBytes& bytes) {
    if (sodium_init() < 0 || std::memcmp(bytes.data(), kJournalMagic, 8) || Get32(bytes.data() + 24) > 1)
        return std::nullopt;
    Digest checksum{};
    if (crypto_hash_sha256(checksum.data(), bytes.data(), kJournalPayload) != 0 ||
        sodium_memcmp(checksum.data(), bytes.data() + kJournalPayload, 32)) return std::nullopt;
    Journal journal;
    journal.partition_bytes = Get32(bytes.data() + 8);
    journal.factory_bytes = Get32(bytes.data() + 12);
    journal.bank_bytes = Get32(bytes.data() + 16);
    journal.high_water = Get32(bytes.data() + 20);
    journal.trial_started = Get32(bytes.data() + 24) != 0;
    std::copy_n(bytes.data() + 28, 32, journal.factory_digest.begin());
    journal.confirmed = GetImage(bytes.data() + 60);
    journal.pending = GetImage(bytes.data() + 100);
    return ValidJournal(journal) ? std::optional<Journal>(journal) : std::nullopt;
}

Store::Store(Layout layout, Digest factory_digest, const asset_release::Compatibility& compatibility,
             std::string public_key_hex, Hooks hooks)
    : layout_(layout), factory_digest_(factory_digest), model_(compatibility.model), sr_abi_(compatibility.sr_abi),
      wake_model_(compatibility.wake_model), key_(std::move(public_key_hex)),
      firmware_sequence_(compatibility.firmware_sequence), hooks_(std::move(hooks)) {
    journal_.partition_bytes = layout_.PartitionBytes();
    journal_.factory_bytes = layout_.FactoryBytes();
    journal_.bank_bytes = layout_.BankBytes();
    journal_.factory_digest = factory_digest_;
    selected_.size = layout_.FactoryBytes();
}
asset_release::Compatibility Store::Compatibility() const { return {model_, sr_abi_, wake_model_, firmware_sequence_}; }
bool Store::HooksReady() const {
    return hooks_.read && hooks_.write && hooks_.erase && hooks_.load_journal && hooks_.save_journal && hooks_.validate;
}
bool Store::Persist(const Journal& next) {
    auto encoded = EncodeJournal(next);
    if (!encoded || !hooks_.save_journal(*encoded)) { faulted_ = true; return false; }
    journal_ = next;
    return true;
}

std::optional<asset_release::Verified> Store::ValidateStored(const Image& image) {
    if (!image.slot || !ValidImage(image)) return std::nullopt;
    std::array<unsigned char, kManifestHeader> header{};
    const auto offset = layout_.ManifestOffset(image.slot);
    if (!hooks_.read(offset, header.data(), header.size()) || std::memcmp(header.data(), kManifestMagic, 8))
        return std::nullopt;
    const auto length = Get32(header.data() + 8);
    if (!length || length > kSectorBytes - kManifestHeader) return std::nullopt;
    std::string envelope(length, '\0');
    if (!hooks_.read(offset + kManifestHeader, envelope.data(), envelope.size())) return std::nullopt;
    // This exception to the update floor is bound to the exact trusted journal identity.
    auto verified = asset_release::Verify(envelope, Compatibility(), image.sequence - 1, key_);
    if (!verified || verified->Sequence() != image.sequence || verified->Size() > layout_.Capacity() ||
        sodium_memcmp(verified->Digest().data(), image.digest.data(), image.digest.size()) ||
        !hooks_.validate(layout_.Offset(image.slot), *verified)) return std::nullopt;
    return verified;
}

Selection Store::Boot() {
    if (booted_) return selected_;
    booted_ = true;
    if (!HooksReady() || Empty(factory_digest_)) { faulted_ = true; return selected_; }
    JournalBytes bytes{};
    const auto loaded = hooks_.load_journal(bytes);
    if (loaded == LoadResult::kMissing) return selected_;
    auto journal = loaded == LoadResult::kLoaded ? DecodeJournal(bytes) : std::nullopt;
    if (!journal || journal->partition_bytes != layout_.PartitionBytes() ||
        journal->factory_bytes != layout_.FactoryBytes() || journal->bank_bytes != layout_.BankBytes() ||
        journal->factory_digest != factory_digest_) { faulted_ = true; return selected_; }
    journal_ = *journal;
    if (journal_.pending.slot) {
        auto candidate = journal_.trial_started ? std::nullopt : ValidateStored(journal_.pending);
        auto next = journal_;
        if (candidate) {
            next.trial_started = true;
            if (Persist(next)) {
                selected_ = {next.pending.slot, layout_.Offset(next.pending.slot), candidate->Size(),
                             next.pending.sequence, true};
                selected_release_ = std::move(candidate);
                return selected_;
            }
        } else {
            next.pending = {}; next.trial_started = false;
            Persist(next);
        }
    }
    if (auto confirmed = ValidateStored(journal_.confirmed)) {
        selected_ = {journal_.confirmed.slot, layout_.Offset(journal_.confirmed.slot), confirmed->Size(),
                     journal_.confirmed.sequence, false};
        selected_release_ = std::move(confirmed);
    }
    return selected_;
}

bool Store::ConfirmTrial(uint32_t sequence) {
    if (!booted_ || faulted_ || !selected_.trial || selected_.sequence != sequence ||
        journal_.pending.slot != selected_.slot || journal_.pending.sequence != sequence || !journal_.trial_started)
        return false;
    auto next = journal_;
    next.confirmed = next.pending; next.pending = {}; next.trial_started = false;
    if (!Persist(next)) return false;
    selected_.trial = false;
    return true;
}
bool Store::RejectTrial(uint32_t sequence) {
    if (!booted_ || faulted_ || !selected_.trial || selected_.sequence != sequence ||
        journal_.pending.slot != selected_.slot || journal_.pending.sequence != sequence) return false;
    auto next = journal_;
    next.pending = {}; next.trial_started = false;
    return Persist(next);
}

bool Store::CanStage() const {
    return booted_ && !faulted_ && !selected_.trial && !journal_.pending.slot && HooksReady();
}

std::optional<asset_release::Verified> Store::VerifyCandidate(std::string_view envelope) const {
    if (!CanStage() || envelope.size() > kSectorBytes - kManifestHeader) return std::nullopt;
    auto release = asset_release::Verify(envelope, Compatibility(), journal_.high_water, key_);
    if (!release || release->Size() > layout_.Capacity()) return std::nullopt;
    return release;
}

StageResult Store::Stage(std::string_view envelope, size_t content_length,
                         const std::function<int(void*, size_t)>& read_body,
                         const std::function<bool()>& cancelled,
                         const std::function<bool()>& before_publish) {
    if (!read_body || !cancelled || !before_publish) return StageResult::kRejected;
    auto release = VerifyCandidate(envelope);
    if (!release || release->Size() != content_length)
        return StageResult::kRejected;
    const unsigned slot = journal_.confirmed.slot == 1 ? 2 : 1;
    if (slot == selected_.slot) return StageResult::kRejected;
    if (cancelled()) return StageResult::kCancelled;
    std::unique_ptr<std::array<unsigned char, kSectorBytes>> buffer(new(std::nothrow) std::array<unsigned char, kSectorBytes>);
    if (!buffer) return StageResult::kIoError;
    const auto start = layout_.Offset(slot);
    const auto manifest = layout_.ManifestOffset(slot);
    if (!hooks_.erase(manifest, kSectorBytes)) return StageResult::kIoError;
    const auto erase_bytes = ((release->Size() + kSectorBytes - 1) / kSectorBytes) * kSectorBytes;
    for (uint32_t offset = 0; offset < erase_bytes; offset += kSectorBytes) {
        if (cancelled()) return StageResult::kCancelled;
        if (!hooks_.erase(start + offset, kSectorBytes)) return StageResult::kIoError;
    }
    size_t written = 0;
    while (written < content_length) {
        const size_t wanted = std::min(content_length - written, buffer->size());
        size_t filled = 0;
        while (filled < wanted) {
            if (cancelled()) return StageResult::kCancelled;
            const int received = read_body(buffer->data() + filled, wanted - filled);
            if (received <= 0 || static_cast<size_t>(received) > wanted - filled) return StageResult::kIoError;
            filled += received;
        }
        if (cancelled()) return StageResult::kCancelled;
        // Encrypted ESP partitions require complete 16-byte writes, even for short HTTP fragments.
        const size_t aligned = (wanted + 15) & ~size_t(15);
        std::fill(buffer->begin() + wanted, buffer->begin() + aligned, 0xff);
        if (!hooks_.write(start + written, buffer->data(), aligned)) return StageResult::kIoError;
        written += wanted;
    }
    if (cancelled()) return StageResult::kCancelled;
    if (read_body(buffer->data(), 1) != 0) return StageResult::kInvalidImage;
    if (cancelled()) return StageResult::kCancelled;
    if (!hooks_.validate(start, *release)) return StageResult::kInvalidImage;
    if (cancelled()) return StageResult::kCancelled;
    std::memcpy(buffer->data(), kManifestMagic, 8);
    Put32(buffer->data() + 8, envelope.size());
    std::memcpy(buffer->data() + kManifestHeader, envelope.data(), envelope.size());
    const auto manifest_bytes = kManifestHeader + envelope.size();
    const auto aligned_manifest_bytes = (manifest_bytes + 15) & ~size_t(15);
    std::fill(buffer->begin() + manifest_bytes, buffer->begin() + aligned_manifest_bytes, 0xff);
    if (!hooks_.write(manifest, buffer->data(), aligned_manifest_bytes)) return StageResult::kIoError;
    Image candidate{slot, release->Sequence(), release->Digest()};
    if (!ValidateStored(candidate)) return StageResult::kInvalidImage;
    if (cancelled() || !before_publish()) return StageResult::kCancelled;
    // The owner pins the commit boundary above; late cancellation cannot undo publication.
    auto next = journal_;
    next.pending = candidate; next.trial_started = false; next.high_water = candidate.sequence;
    return Persist(next) ? StageResult::kStaged : StageResult::kPersistenceUncertain;
}
}  // namespace asset_storage
