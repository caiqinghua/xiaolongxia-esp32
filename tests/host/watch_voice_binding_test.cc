#include "watch/watch_voice_binding.h"
#include "nvs.h"
#include <cassert>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace watch_voice;
const std::string device = "12345678-1234-1234-1234-123456789abc";
const std::string credential = "adv_" + std::string(43, 'x');
std::vector<uint8_t> disk, receipt_disk, pending;
bool receipt_present = false, pending_receipt = false;
bool present = false, encrypted = true, fail_open = false, fail_read = false;
bool fail_write = false, fail_commit = false;
unsigned handles = 0, reads = 0, writes = 0, commits = 0;
nvs_open_mode_t mode;

Binding Load(BindingStatus expected) {
    Binding result{999, "stale", "stale"};
    assert(LoadBinding(result) == expected);
    assert(ActiveBindingGeneration() == (expected == BindingStatus::kBound ? result.generation : 0));
    assert(handles == 0);
    if (expected != BindingStatus::kBound && expected != BindingStatus::kRemoved) {
        assert(result.generation == 0 && result.device_id.empty() && result.credential.empty());
    }
    return result;
}
}

bool esp_flash_encryption_enabled() { return encrypted; }
esp_err_t nvs_open(const char* name, nvs_open_mode_t requested, nvs_handle_t* handle) {
    assert(std::string(name) == "audream_voice" && handles == 0);
    if (fail_open) return ESP_FAIL;
    if (requested == NVS_READONLY && !present) return ESP_ERR_NVS_NOT_FOUND;
    ++handles;
    *handle = 1;
    mode = requested;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {
    assert(handle == 1 && handles == 1);
    --handles;
    pending.clear();
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out, size_t* size) {
    assert(handle == 1 && handles == 1);
    const bool receipt = std::string(key) == "settings_v1";
    assert(receipt || std::string(key) == "binding_v1");
    const auto& source = receipt ? receipt_disk : disk;
    if (fail_read) return ESP_FAIL;
    if (!(receipt ? receipt_present : present)) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *size = source.size(); return ESP_OK; }
    ++reads;
    if (*size < source.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(out, source.data(), source.size());
    *size = source.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* data, size_t size) {
    assert(handle == 1 && handles == 1 && mode == NVS_READWRITE);
    pending_receipt = std::string(key) == "settings_v1";
    if (!pending_receipt) assert(ActiveBindingGeneration() == 0);
    assert(pending_receipt ? size == 64 : (std::string(key) == "binding_v1" && size == 176));
    ++writes;
    if (fail_write) return ESP_FAIL;
    const auto* bytes = static_cast<const uint8_t*>(data);
    pending.assign(bytes, bytes + size);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1 && mode == NVS_READWRITE);
    ++commits;
    // Simulate storage changing even if commit reports failure.
    if (pending_receipt) {
        receipt_disk = pending;
        receipt_present = true;
    } else {
        disk = pending;
        present = true;
    }
    return fail_commit ? ESP_FAIL : ESP_OK;
}

int main(int argc, char** argv) {
    using namespace watch_voice;
    Load(BindingStatus::kAbsent);
    bool verifier_called = false;
    assert(!WithBoundIdentity([&](std::string_view, uint64_t) { verifier_called = true; return true; }));
    assert(!verifier_called && ActiveBindingGeneration() == 0);
#if !CONFIG_NVS_ENCRYPTION || !CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC
    (void)argc;
    (void)argv;
    assert(SaveBinding(0, device, credential) == BindingStatus::kProtectionRequired);
    assert(RemoveBinding(0) == BindingStatus::kProtectionRequired);
    SettingsReceipt receipt;
    const Binding expected{1, device, credential};
    assert(LoadSettingsReceipt(expected, receipt) == ReceiptStatus::kProtectionRequired);
    assert(SaveSettingsReceipt(expected, 1, true, {}, receipt) == ReceiptStatus::kProtectionRequired);
    assert(writes == 0 && commits == 0);
    present = true;
    disk.resize(176);
    Load(BindingStatus::kProtectionRequired);
    assert(!WithBoundIdentity([&](std::string_view, uint64_t) { verifier_called = true; return true; }));
    assert(!verifier_called && ActiveBindingGeneration() == 0);
    assert(reads == 0);
    std::puts("PASS: unprotected build cannot save or load binding secrets");
#else
    if (argc > 2 && std::string(argv[1]) == "receipt-reboot") {
        std::ifstream input(argv[2], std::ios::binary);
        const std::vector<uint8_t> saved{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        assert(input && saved.size() == 240);
        disk.assign(saved.begin(), saved.begin() + 176);
        receipt_disk.assign(saved.begin() + 176, saved.end());
        present = receipt_present = true;
        const auto binding = Load(BindingStatus::kBound);
        assert(binding.generation == 3);
        SettingsReceipt restored;
        assert(LoadSettingsReceipt(binding, restored) == ReceiptStatus::kOk);
        assert(restored.revision == 3 && restored.applied_revision == 3 && restored.enabled && restored.error.empty());
        assert(restored.power_mode == "battery_saver" && restored.applied_power_mode == "battery_saver");
        assert(writes == 0 && commits == 0 && handles == 0);
        std::puts("PASS: fresh process restores receipt from serialized NVS without writing or authorization");
        return 0;
    }
    encrypted = false;
    assert(SaveBinding(0, device, credential) == BindingStatus::kProtectionRequired);
    assert(writes == 0);
    encrypted = true;
    assert(SaveBinding(0, device, "legacy-token") == BindingStatus::kInvalid);
    assert(SaveBinding(0, "00:11:22:33:44:55", credential) == BindingStatus::kInvalid);
    assert(SaveBindingAuthorized(0, device, credential, {}) == BindingStatus::kInvalid);
    assert(SaveBindingAuthorized(0, device, credential, [] { return false; }) == BindingStatus::kCancelled);
    assert(writes == 0 && commits == 0 && !present && handles == 0);
    assert(SaveBindingAuthorized(0, device, credential, [] { return true; }) == BindingStatus::kBound);
    const auto first = Load(BindingStatus::kBound);
    assert(first.generation == 1 && first.device_id == device && first.credential == credential);
    assert(WithBoundIdentity([&](std::string_view id, uint64_t generation) {
        return id == device && generation == first.generation && ActiveBindingGeneration() == generation;
    }));
    assert(!WithBoundIdentity([](std::string_view, uint64_t) { return false; }));
    assert(ActiveBindingGeneration() == first.generation);
    const auto before_cancel = disk;
    const auto writes_before_cancel = writes;
    assert(SaveBindingAuthorized(1, device, credential, [] { return false; }) == BindingStatus::kCancelled);
    assert(disk == before_cancel && writes == writes_before_cancel && ActiveBindingGeneration() == 1);
    bool admitted = false;
    assert(SaveBindingAuthorized(0, device, credential, [&] { admitted = true; return true; }) == BindingStatus::kConflict);
    assert(!admitted);
    std::puts("PASS: cancelled admission leaves durable binding and active generation unchanged");
    SettingsReceipt receipt;
    assert(LoadSettingsReceipt(first, receipt) == ReceiptStatus::kAbsent);
    assert(SaveSettingsReceipt(first, 2, true, {}, receipt) == ReceiptStatus::kOk);
    assert(receipt.revision == 2 && receipt.applied_revision == 2 && receipt.error.empty());
    assert(LoadSettingsReceipt(first, receipt) == ReceiptStatus::kOk);
    receipt_disk[0] = 1;
    assert(LoadSettingsReceipt(first, receipt) == ReceiptStatus::kOk);
    assert(receipt.power_mode == "watch_standby" && receipt.applied_power_mode == "watch_standby");
    const auto written = writes;
    assert(SaveSettingsReceipt(first, 2, true, {}, receipt) == ReceiptStatus::kOk);
    assert(writes == written);
    assert(SaveSettingsReceipt(first, 1, false, {}, receipt) == ReceiptStatus::kConflict);
    assert(SaveSettingsReceipt(first, 2, false, {}, receipt) == ReceiptStatus::kConflict);
    assert(SaveSettingsReceipt(first, 3, true, "local_disabled", receipt) == ReceiptStatus::kOk);
    assert(receipt.applied_revision == 2 && receipt.error == "local_disabled");
    assert(SaveSettingsReceipt(first, 3, true, {}, receipt) == ReceiptStatus::kOk);
    assert(receipt.applied_revision == 3 && receipt.error.empty());
    assert(SaveSettingsReceipt(first, 3, true, "application_failed", receipt) == ReceiptStatus::kOk);
    assert(receipt.applied_revision == 3 && receipt.error == "application_failed");
    assert(SaveSettingsReceipt(first, 0, true, {}, receipt) == ReceiptStatus::kInvalid);
    assert(SaveSettingsReceipt(first, 4, true, "arbitrary", receipt) == ReceiptStatus::kInvalid);
    std::puts("PASS: durable receipt preserves applied history, rejects rollback and skips unchanged writes");
    assert(SaveSettingsReceipt(first, 4, true, {}, receipt, "battery_saver") == ReceiptStatus::kOk);
    assert(LoadSettingsReceipt(first, receipt) == ReceiptStatus::kOk);
    assert(receipt.power_mode == "battery_saver" && receipt.applied_power_mode == "battery_saver");
    assert(SaveSettingsReceipt(first, 4, true, {}, receipt, "watch_standby") == ReceiptStatus::kConflict);
    assert(SaveSettingsReceipt(first, 5, true, "unsupported_mode", receipt, "always_available") == ReceiptStatus::kOk);
    assert(receipt.applied_revision == 4 && receipt.applied_power_mode == "battery_saver");
    assert(SaveSettingsReceipt(first, 6, true, {}, receipt, "unknown") == ReceiptStatus::kInvalid);
    std::puts("PASS: power mode survives serialization, revision conflicts and failed newer settings");
    assert(SaveBinding(0, device, credential) == BindingStatus::kConflict);
    assert(RemoveBinding(0) == BindingStatus::kConflict);
    std::thread remover;
    std::atomic<bool> removing{false}, removed_done{false};
    assert(WithBoundIdentity([&](std::string_view id, uint64_t generation) {
        remover = std::thread([&] {
            removing.store(true);
            assert(RemoveBinding(1) == BindingStatus::kRemoved);
            removed_done.store(true);
        });
        while (!removing.load()) std::this_thread::yield();
        assert(!removed_done.load() && id == device && generation == 1 && ActiveBindingGeneration() == 1);
        return true;
    }));
    remover.join();
    assert(removed_done.load() && ActiveBindingGeneration() == 0);
    assert(!WithBoundIdentity([](std::string_view, uint64_t) { return true; }));
    const auto removed = Load(BindingStatus::kRemoved);
    assert(removed.generation == 2 && removed.device_id.empty() && removed.credential.empty());
    disk[12] = 'x';
    Load(BindingStatus::kStorageError);
    disk[12] = 0;
    Load(BindingStatus::kRemoved);
    assert(SaveBinding(1, device, credential) == BindingStatus::kConflict);
    assert(SaveBinding(2, device, credential) == BindingStatus::kBound);
    assert(Load(BindingStatus::kBound).generation == 3);
    assert(LoadSettingsReceipt(first, receipt) == ReceiptStatus::kConflict);
    assert(SaveSettingsReceipt(first, 4, true, {}, receipt) == ReceiptStatus::kConflict);
    const auto rebound = Load(BindingStatus::kBound);
    assert(LoadSettingsReceipt(rebound, receipt) == ReceiptStatus::kAbsent);
    assert(SaveSettingsReceipt(rebound, 1, false, {}, receipt) == ReceiptStatus::kOk);
    assert(receipt.revision == 1 && receipt.applied_revision == 1 && !receipt.enabled);
    std::puts("PASS: receipt belongs to one binding generation and cannot authorize a rebound device");
    std::puts("PASS: atomic binding, removal tombstone and generation fencing");

    const auto good = disk;
    for (size_t index : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(12), size_t(48), size_t(175)}) {
        disk = good;
        disk[index] = 255;
        Load(BindingStatus::kStorageError);
    }
    for (size_t size : {size_t(0), size_t(1), size_t(175), size_t(177), size_t(8192)}) {
        disk.resize(size);
        const auto before = reads;
        Load(BindingStatus::kStorageError);
        assert(reads == before);
    }
    disk = good;
    for (size_t index = 4; index < 12; ++index) disk[index] = 0;
    Load(BindingStatus::kStorageError);
    for (size_t index = 4; index < 12; ++index) disk[index] = 255;
    assert(Load(BindingStatus::kBound).generation == std::numeric_limits<uint64_t>::max());
    assert(RemoveBinding(std::numeric_limits<uint64_t>::max()) == BindingStatus::kConflict);
    disk = good;
    encrypted = false;
    const auto before = reads;
    Load(BindingStatus::kProtectionRequired);
    assert(reads == before);
    encrypted = true;
    fail_open = true;
    Load(BindingStatus::kStorageError);
    fail_open = false;
    fail_read = true;
    Load(BindingStatus::kStorageError);
    fail_read = false;
    assert(Load(BindingStatus::kBound).generation == 3);
    std::puts("PASS: bounded reads, corrupt records and protection loss fail closed");

    const auto valid_receipt = receipt_disk;
    for (const size_t index : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(12), size_t(56)}) {
        receipt_disk = valid_receipt;
        receipt_disk[index] = 255;
        assert(LoadSettingsReceipt(rebound, receipt) == ReceiptStatus::kStorageError);
        assert(receipt.revision == 0 && receipt.applied_revision == 0 && receipt.error.empty());
    }
    for (const size_t size : {size_t(0), size_t(63), size_t(65), size_t(8192)}) {
        receipt_disk.resize(size);
        assert(LoadSettingsReceipt(rebound, receipt) == ReceiptStatus::kStorageError);
    }
    receipt_disk = valid_receipt;
    assert(LoadSettingsReceipt(rebound, receipt) == ReceiptStatus::kOk);
    assert(receipt.revision == 1 && !receipt.enabled);
    std::puts("PASS: corrupt receipt does not expose stale application history");

    encrypted = false;
    const auto reads_before = reads;
    assert(LoadSettingsReceipt(rebound, receipt) == ReceiptStatus::kProtectionRequired);
    assert(SaveSettingsReceipt(rebound, 2, true, {}, receipt) == ReceiptStatus::kProtectionRequired);
    assert(reads == reads_before);
    encrypted = true;
    auto wrong_credential = rebound;
    wrong_credential.credential += "x";
    assert(SaveSettingsReceipt(wrong_credential, 2, true, {}, receipt) == ReceiptStatus::kConflict);
    ReceiptStatus first_write{}, second_write{};
    std::thread older([&]() {
        SettingsReceipt result;
        first_write = SaveSettingsReceipt(rebound, 2, true, {}, result);
    });
    std::thread newer([&]() {
        SettingsReceipt result;
        second_write = SaveSettingsReceipt(rebound, 3, true, {}, result, "battery_saver");
    });
    older.join();
    newer.join();
    assert(first_write == ReceiptStatus::kOk || first_write == ReceiptStatus::kConflict);
    assert(second_write == ReceiptStatus::kOk);
    assert(LoadSettingsReceipt(rebound, receipt) == ReceiptStatus::kOk);
    assert(receipt.revision == 3 && receipt.applied_revision == 3 && receipt.enabled);
    std::puts("PASS: protected receipt updates serialize and retain the newest applied revision");

    if (argc > 2 && std::string(argv[1]) == "receipt-export") {
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(disk.data()), disk.size());
        output.write(reinterpret_cast<const char*>(receipt_disk.data()), receipt_disk.size());
        output.close();
        assert(output);
        return 0;
    }

    if (argc > 1 && std::string(argv[1]).find("receipt-") == 0) {
        if (std::string(argv[1]) == "receipt-write") fail_write = true;
        else fail_commit = true;
        assert(SaveSettingsReceipt(rebound, 4, true, {}, receipt) == ReceiptStatus::kStorageError);
        fail_write = fail_commit = false;
        assert(LoadSettingsReceipt(rebound, receipt) == ReceiptStatus::kStorageError);
        assert(SaveSettingsReceipt(rebound, 5, true, {}, receipt) == ReceiptStatus::kStorageError);
        assert(Load(BindingStatus::kBound).generation == rebound.generation);
        assert(handles == 0);
        std::puts("PASS: uncertain receipt writes block history reuse without modifying credentials");
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "write") fail_write = true;
    else fail_commit = true;
    assert(RemoveBinding(3) == BindingStatus::kStorageError);
    fail_write = fail_commit = false;
    Load(BindingStatus::kStorageError);
    assert(SaveBinding(3, device, credential) == BindingStatus::kStorageError);
    assert(LoadSettingsReceipt(rebound, receipt) == ReceiptStatus::kStorageError);
    assert(SaveSettingsReceipt(rebound, 4, true, {}, receipt) == ReceiptStatus::kStorageError);
    assert(handles == 0);
    std::puts("PASS: uncertain writes prohibit all credential reuse for this boot");
#endif
}
