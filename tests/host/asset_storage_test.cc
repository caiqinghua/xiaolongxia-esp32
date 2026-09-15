#include "asset_storage.h"
#include "asset_release_fixture.h"
#include <cstring>
#include <iostream>

namespace {
using namespace asset_fixture;
using namespace asset_storage;
struct Flash {
    Bytes factory = Bundle();
    Layout layout = *Layout::Create(512 * 1024, factory.size());
    Digest factory_digest{};
    Bytes memory = Bytes(layout.PartitionBytes(), 0xff);
    std::optional<JournalBytes> journal;
    int operation = 0, fail_operation = -1;
    bool load_error = false, save_error = false, persist_despite_error = false;
    bool corrupt_manifest_write = false, corrupt_payload_write = false;
    bool cancelled = false;
    std::function<void()> after_write;
    unsigned protected_slot = 0;
    Flash() {
        std::copy(factory.begin(), factory.end(), memory.begin());
        assert(crypto_hash_sha256(factory_digest.data(), factory.data(), factory.size()) == 0);
    }
    bool Step() { return ++operation != fail_operation; }
    void CheckRange(uint32_t offset, size_t size, bool writing) {
        assert(offset <= memory.size() && size <= memory.size() - offset);
        if (writing) {
            assert(offset >= layout.Offset(1));
            if (protected_slot) assert(offset + size <= layout.Offset(protected_slot) ||
                offset >= layout.Offset(protected_slot) + layout.BankBytes());
        }
    }
    Hooks MakeHooks() {
        return {
            [this](uint32_t offset, void* bytes, size_t size) {
                CheckRange(offset, size, false);
                if (!Step()) return false;
                std::memcpy(bytes, memory.data() + offset, size); return true;
            },
            [this](uint32_t offset, const void* bytes, size_t size) {
                CheckRange(offset, size, true);
                assert(offset % 16 == 0 && size % 16 == 0);
                if (!Step()) return false;
                const auto* source = static_cast<const unsigned char*>(bytes);
                for (size_t i = 0; i < size; ++i) {
                    assert((memory[offset + i] & source[i]) == source[i]);
                    memory[offset + i] = source[i];
                }
                if (corrupt_manifest_write && (offset == layout.ManifestOffset(1) || offset == layout.ManifestOffset(2)))
                    memory[offset] ^= 1;
                if (corrupt_payload_write && (offset == layout.Offset(1) || offset == layout.Offset(2))) memory[offset] ^= 1;
                if (after_write) after_write();
                return true;
            },
            [this](uint32_t offset, size_t size) {
                CheckRange(offset, size, true);
                assert(offset % kSectorBytes == 0 && size % kSectorBytes == 0);
                if (!Step()) return false;
                std::fill_n(memory.begin() + offset, size, 0xff); return true;
            },
            [this](JournalBytes& bytes) {
                if (load_error || !Step()) return LoadResult::kError;
                if (!journal) return LoadResult::kMissing;
                bytes = *journal; return LoadResult::kLoaded;
            },
            [this](const JournalBytes& bytes) {
                if (!Step()) return false;
                if (!save_error || persist_despite_error) journal = bytes;
                return !save_error;
            },
            [this](uint32_t offset, const asset_release::Verified& release) {
                CheckRange(offset, release.Size(), false);
                return Step() && asset_release::ValidateBundle(release, memory.data() + offset, release.Size());
            }
        };
    }
    Store NewStore(uint32_t firmware_sequence = 7) {
        auto compatibility = kCompatibility;
        compatibility.firmware_sequence = firmware_sequence;
        return Store(layout, factory_digest, compatibility, PublicKey(), MakeHooks());
    }
    std::string Envelope(uint32_t sequence, const Bytes& data) {
        return Sign(Change(Payload(data), "sequence", std::to_string(sequence)));
    }
    StageResult Stage(Store& store, uint32_t sequence, Bytes data = Bundle(), bool publish = true,
                      bool trailing = false, bool short_body = false) {
        size_t cursor = 0;
        const auto envelope = Envelope(sequence, data);
        return store.Stage(envelope, data.size(), [&](void* destination, size_t maximum) {
            if (short_body && cursor >= data.size() / 2) return 0;
            if (cursor == data.size()) return trailing ? 1 : 0;
            const auto size = std::min({maximum, data.size() - cursor, size_t(31)});
            std::memcpy(destination, data.data() + cursor, size); cursor += size;
            return static_cast<int>(size);
        }, [&] { return cancelled; }, [publish] { return publish; });
    }
    void FactoryIntact() const { assert(std::equal(factory.begin(), factory.end(), memory.begin())); }
    void InstallConfirmed(uint32_t sequence = 8) {
        auto initial = NewStore(); assert(initial.Boot().slot == 0);
        assert(Stage(initial, sequence) == StageResult::kStaged);
        auto trial = NewStore(); assert(trial.Boot().trial);
        assert(trial.ConfirmTrial(sequence));
        auto confirmed = NewStore(); protected_slot = confirmed.Boot().slot;
        assert(protected_slot && !confirmed.Selected().trial);
    }
};

void TestLayoutAndJournal() {
    assert(!Layout::Create(16 * 1024 * 1024, 16 * 1024 * 1024));
    assert(!Layout::Create(UINT32_MAX, 12));
    assert(!Layout::Create(65536, 12));
    assert(!Layout::Create(512 * 1024 + 1, 12));
    assert(!Layout::Create(512 * 1024, 11));
    for (uint32_t bytes : {12U, 65535U, 65536U, 65537U, 300000U}) {
        auto layout = Layout::Create(512 * 1024, bytes); assert(layout);
        assert(layout->Offset(1) >= bytes && layout->Offset(1) % kMapAlignment == 0);
        assert(layout->Offset(2) == layout->Offset(1) + layout->BankBytes());
        assert(layout->ManifestOffset(2) + kSectorBytes <= layout->PartitionBytes());
        assert(layout->Offset(0) == 0 && layout->ManifestOffset(3) == 0);
    }
    Flash flash;
    auto initial = flash.NewStore(); initial.Boot();
    assert(flash.Stage(initial, 8) == StageResult::kStaged);
    assert(flash.journal);
    assert(EncodeJournal(*DecodeJournal(*flash.journal)) == flash.journal);
    for (size_t i = 0; i < kJournalBytes; ++i) {
        auto corrupt = *flash.journal; corrupt[i] ^= 1;
        assert(!DecodeJournal(corrupt));
    }
    auto bad = *DecodeJournal(*flash.journal); bad.pending.slot = 3; assert(!EncodeJournal(bad));
    bad = *DecodeJournal(*flash.journal); bad.high_water = 7; assert(!EncodeJournal(bad));
    bad = *DecodeJournal(*flash.journal); bad.confirmed = bad.pending; assert(!EncodeJournal(bad));
    bad = *DecodeJournal(*flash.journal); bad.pending = {}; bad.trial_started = true; assert(!EncodeJournal(bad));
    std::cout << "PASS: aligned immutable factory layout and canonical corruption-detecting journal\n";
}

void TestTrialAndRollback() {
    Flash flash;
    auto initial = flash.NewStore(); assert(initial.Boot().slot == 0);
    assert(!initial.ConfirmTrial(8));
    assert(flash.Stage(initial, 8) == StageResult::kStaged);
    assert(initial.Selected().slot == 0 && initial.HighWater() == 8);
    assert(flash.Stage(initial, 9) == StageResult::kRejected);
    auto trial = flash.NewStore(); auto selected = trial.Boot();
    assert(selected.slot == 1 && selected.trial && selected.sequence == 8);
    assert(trial.Boot().slot == 1); // One instance is one boot, not a second trial attempt.
    assert(!trial.ConfirmTrial(9));
    assert(flash.Stage(trial, 9) == StageResult::kRejected);
    auto failed = flash.NewStore(); assert(failed.Boot().slot == 0 && failed.HighWater() == 8);
    assert(flash.Stage(failed, 8) == StageResult::kRejected);
    assert(flash.Stage(failed, 9) == StageResult::kStaged);
    auto second = flash.NewStore(); assert(second.Boot().trial && second.ConfirmTrial(9));
    auto stable = flash.NewStore(); assert(stable.Boot().slot == 1 && !stable.Selected().trial);
    flash.protected_slot = 1;
    const auto protected_memory = flash.memory;
    assert(flash.Stage(stable, 10) == StageResult::kStaged);
    assert(std::equal(protected_memory.begin(), protected_memory.begin() + flash.layout.Offset(2), flash.memory.begin()));
    auto third = flash.NewStore(); assert(third.Boot().slot == 2 && third.Selected().trial);
    assert(third.RejectTrial(10));
    assert(!third.ConfirmTrial(10) && flash.Stage(third, 11) == StageResult::kRejected);
    auto restored = flash.NewStore(); assert(restored.Boot().slot == 1 && restored.HighWater() == 10);
    assert(flash.Stage(restored, 10) == StageResult::kRejected);
    flash.FactoryIntact();
    std::cout << "PASS: staged assets require reboot and health confirmation; failed trials retain the old bank\n";
}

void TestFailureBoundaries() {
    Flash baseline; baseline.InstallConfirmed();
    auto measure = baseline;
    auto ready = measure.NewStore(); ready.Boot();
    measure.operation = 0;
    assert(measure.Stage(ready, 9) == StageResult::kStaged);
    const int count = measure.operation;
    for (int failure = 1; failure <= count; ++failure) {
        auto flash = baseline;
        auto store = flash.NewStore(); assert(store.Boot().slot == 1);
        flash.operation = 0; flash.fail_operation = failure;
        assert(flash.Stage(store, 9) != StageResult::kStaged);
        flash.fail_operation = -1;
        auto reboot = flash.NewStore(); assert(reboot.Boot().slot == 1 && !reboot.Selected().trial);
        flash.FactoryIntact();
    }
    for (bool uncertain : {false, true}) {
        auto flash = baseline; auto store = flash.NewStore(); store.Boot();
        flash.save_error = true; flash.persist_despite_error = uncertain;
        assert(flash.Stage(store, 9) == StageResult::kPersistenceUncertain && store.Faulted());
        assert(flash.Stage(store, 10) == StageResult::kRejected);
        flash.save_error = false;
        auto reboot = flash.NewStore(); auto selected = reboot.Boot();
        assert(selected.slot == (uncertain ? 2U : 1U));
        if (uncertain) {
            assert(selected.trial);
            auto failed = flash.NewStore(); assert(failed.Boot().slot == 1);
        }
    }
    for (bool uncertain : {false, true}) {
        auto flash = baseline; auto store = flash.NewStore(); store.Boot();
        assert(flash.Stage(store, 9) == StageResult::kStaged);
        flash.save_error = true; flash.persist_despite_error = uncertain;
        auto trial = flash.NewStore(); assert(trial.Boot().slot == 1 && trial.Faulted());
        assert(!trial.ConfirmTrial(9));
    }
    {
        auto flash = baseline; auto store = flash.NewStore(); store.Boot();
        assert(flash.Stage(store, 9) == StageResult::kStaged);
        auto trial = flash.NewStore(); assert(trial.Boot().trial);
        flash.save_error = true; assert(!trial.ConfirmTrial(9) && trial.Faulted());
        flash.save_error = false;
        auto reboot = flash.NewStore(); assert(reboot.Boot().slot == 1);
    }
    std::cout << "PASS: every staging I/O failure and uncertain journal publication preserves recovery\n";
}

void TestCancellationAndCorruption() {
    for (int mode = 0; mode < 7; ++mode) {
        Flash flash; flash.InstallConfirmed();
        auto store = flash.NewStore(); store.Boot();
        if (mode == 0) flash.cancelled = true;
        if (mode == 1) flash.after_write = [&] { flash.cancelled = true; };
        if (mode == 2) flash.corrupt_manifest_write = true;
        if (mode == 3) flash.corrupt_payload_write = true;
        assert(flash.Stage(store, 9, Bundle(), mode != 4, mode == 5, mode == 6) != StageResult::kStaged);
        assert(DecodeJournal(*flash.journal)->pending.slot == 0);
        flash.FactoryIntact();
    }
    Flash flash; flash.InstallConfirmed();
    auto store = flash.NewStore(); store.Boot();
    const auto data = Bundle(); size_t cursor = 0;
    assert(store.Stage(flash.Envelope(9, data), data.size(), [&](void* bytes, size_t size) {
        size = std::min(size, data.size() - cursor);
        std::memcpy(bytes, data.data() + cursor, size); cursor += size; return static_cast<int>(size);
    }, [&] { return flash.cancelled; }, [&] { flash.cancelled = true; return true; }) == StageResult::kStaged);
    assert(DecodeJournal(*flash.journal)->pending.sequence == 9); // Cancellation after commit admission is late.
    flash.memory[flash.layout.Offset(2)] ^= 1;
    auto corrupt_trial = flash.NewStore(); assert(corrupt_trial.Boot().slot == 1);
    flash.memory[flash.layout.Offset(1)] ^= 1;
    auto corrupt_confirmed = flash.NewStore(); assert(corrupt_confirmed.Boot().slot == 0);
    flash.FactoryIntact();
    std::cout << "PASS: cancellation, length faults, readback corruption and late commit cancellation\n";
}

void TestCompatibilityAndStateFaults() {
    Flash baseline; baseline.InstallConfirmed();
    for (uint32_t firmware : {5U, 10U}) {
        auto flash = baseline;
        auto old = flash.NewStore(firmware); assert(old.Boot().slot == 0);
        assert(old.HighWater() == 8 && flash.Stage(old, 9) == StageResult::kRejected);
        auto supported = flash.NewStore(); assert(supported.Boot().slot == 1);
    }
    for (int mode = 0; mode < 4; ++mode) {
        auto flash = baseline;
        if (mode == 0) flash.load_error = true;
        if (mode == 1) (*flash.journal)[70] ^= 1;
        if (mode == 2) flash.factory_digest[0] ^= 1;
        if (mode == 3) flash.factory_digest.fill(0);
        auto broken = flash.NewStore(); assert(broken.Boot().slot == 0 && broken.Faulted());
        assert(flash.Stage(broken, 9) == StageResult::kRejected);
    }
    auto flash = baseline; auto store = flash.NewStore(); store.Boot();
    const auto calls = flash.operation;
    const auto envelope = flash.Envelope(9, Bundle());
    assert(store.Stage(envelope, 1, [](void*, size_t) { return 0; }, [] { return false; }, [] { return true; }) == StageResult::kRejected);
    assert(store.Stage(envelope, Bundle().size(), [](void*, size_t) { return 0; }, [] { return false; }, {}) == StageResult::kRejected);
    assert(flash.operation == calls);
    auto oversized_payload = Change(Payload(Bundle()), "sequence", "9");
    oversized_payload = Change(oversized_payload, "size", std::to_string(flash.layout.Capacity() + 1));
    assert(store.Stage(Sign(oversized_payload), flash.layout.Capacity() + 1, [](void*, size_t) { return 0; },
                       [] { return false; }, [] { return true; }) == StageResult::kRejected);
    assert(flash.operation == calls);
    std::cout << "PASS: firmware rollback uses factory fallback; damaged trusted state cannot reset the update floor\n";
}

void TestFragmentedEncryptedWriteBoundaries() {
    const std::string index = R"({"version":1,"srmodels":"srmodels.bin"})";
    const auto base_size = Bundle(index).size();
    for (size_t size : {4095U, 4096U, 4097U, 8191U, 8192U, 8193U}) {
        Flash flash;
        auto data = Bundle(index + std::string(size - base_size, ' '));
        assert(data.size() == size);
        auto store = flash.NewStore(); store.Boot();
        assert(flash.Stage(store, 8, data) == StageResult::kStaged);
        auto trial = flash.NewStore(); assert(trial.Boot().trial && trial.Selected().size == size);
        assert(std::equal(data.begin(), data.end(), flash.memory.begin() + trial.Selected().offset));
        const auto aligned = (size + 15) & ~size_t(15);
        for (size_t i = size; i < aligned; ++i) assert(flash.memory[trial.Selected().offset + i] == 0xff);
        assert(trial.ConfirmTrial(8)); flash.FactoryIntact();
    }
    std::cout << "PASS: fragmented HTTP input and exact sector boundaries use aligned encrypted writes\n";
}
}  // namespace

int main() {
    assert(sodium_init() >= 0);
    TestLayoutAndJournal();
    TestTrialAndRollback();
    TestFailureBoundaries();
    TestCancellationAndCorruption();
    TestCompatibilityAndStateFaults();
    TestFragmentedEncryptedWriteBoundaries();
}
