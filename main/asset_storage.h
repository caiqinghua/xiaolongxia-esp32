#pragma once

#include "asset_release.h"
#include <array>
#include <functional>

namespace asset_storage {
constexpr uint32_t kMapAlignment = 65536;
constexpr uint32_t kSectorBytes = 4096;
constexpr size_t kJournalBytes = 172;
using Digest = std::array<unsigned char, 32>;
using JournalBytes = std::array<unsigned char, kJournalBytes>;

// Factory data never moves. Two optional banks occupy only its aligned tail.
class Layout {
public:
    static std::optional<Layout> Create(uint32_t partition_bytes, uint32_t factory_bytes);
    uint32_t PartitionBytes() const { return partition_bytes_; }
    uint32_t FactoryBytes() const { return factory_bytes_; }
    uint32_t BankBytes() const { return bank_bytes_; }
    uint32_t Capacity() const { return bank_bytes_ - kSectorBytes; }
    uint32_t Offset(unsigned slot) const;
    uint32_t ManifestOffset(unsigned slot) const;
private:
    Layout() = default;
    uint32_t partition_bytes_ = 0, factory_bytes_ = 0, first_bank_ = 0, bank_bytes_ = 0;
};

struct Image {
    uint32_t slot = 0;
    uint32_t sequence = 0;
    Digest digest{};
};
struct Journal {
    uint32_t partition_bytes = 0, factory_bytes = 0, bank_bytes = 0, high_water = 0;
    bool trial_started = false;
    Digest factory_digest{};
    Image confirmed, pending;
};
std::optional<JournalBytes> EncodeJournal(const Journal& journal);
std::optional<Journal> DecodeJournal(const JournalBytes& bytes);

enum class LoadResult { kMissing, kLoaded, kError };
struct Hooks {
    std::function<bool(uint32_t offset, void* bytes, size_t size)> read;
    std::function<bool(uint32_t offset, const void* bytes, size_t size)> write;
    std::function<bool(uint32_t offset, size_t size)> erase;
    std::function<LoadResult(JournalBytes& bytes)> load_journal;
    // Must atomically persist one blob and verify it. False may mean uncertain commit.
    std::function<bool(const JournalBytes& bytes)> save_journal;
    // Map the exact range and call asset_release::ValidateBundle, then unmap it.
    std::function<bool(uint32_t offset, const asset_release::Verified& release)> validate;
};

struct Selection {
    uint32_t slot = 0, offset = 0, size = 0, sequence = 0;
    bool trial = false;
};
enum class StageResult { kStaged, kRejected, kCancelled, kIoError, kInvalidImage, kPersistenceUncertain };

// A serialized owner calls this before exposing mapped assets to consumers.
// I/O hooks are synchronous; cancellation is the only cross-thread callback.
class Store {
public:
    Store(Layout layout, Digest factory_digest, const asset_release::Compatibility& compatibility,
          std::string public_key_hex, Hooks hooks);
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Selection Boot();
    // Call only after the selected model and display have passed actual health checks.
    bool ConfirmTrial(uint32_t sequence);
    // Keep the failed mapping alive until reboot; it cannot become a staging target.
    bool RejectTrial(uint32_t sequence);
    bool CanStage() const;
    std::optional<asset_release::Verified> VerifyCandidate(std::string_view envelope) const;
    StageResult Stage(std::string_view envelope, size_t content_length,
                      const std::function<int(void*, size_t)>& read_body,
                      const std::function<bool()>& cancelled,
                      const std::function<bool()>& before_publish);
    bool Faulted() const { return faulted_; }
    uint32_t HighWater() const { return journal_.high_water; }
    const Selection& Selected() const { return selected_; }
    const asset_release::Verified* SelectedRelease() const { return selected_release_ ? &*selected_release_ : nullptr; }
private:
    bool Persist(const Journal& next);
    std::optional<asset_release::Verified> ValidateStored(const Image& image);
    asset_release::Compatibility Compatibility() const;
    bool HooksReady() const;

    Layout layout_;
    Digest factory_digest_;
    std::string model_, sr_abi_, wake_model_, key_;
    uint32_t firmware_sequence_;
    Hooks hooks_;
    Journal journal_;
    Selection selected_;
    std::optional<asset_release::Verified> selected_release_;
    bool booted_ = false, faulted_ = false;
};
}  // namespace asset_storage
