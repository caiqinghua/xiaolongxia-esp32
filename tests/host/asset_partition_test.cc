#include "asset_partition.h"
#include "asset_mapping.h"
#include "asset_release_fixture.h"
#include "asset_update_client.h"
#include "watch/watch_firmware_update.h"
#include "http.h"
#include <nvs.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <set>

namespace {
using namespace asset_storage;
using namespace asset_fixture;
Bytes memory, original;
std::optional<Bytes> committed, pending;
unsigned maps = 0, handles = 0, erases = 0, writes = 0;
std::set<esp_partition_mmap_handle_t> mapped_handles;
unsigned map_calls = 0, fail_map_call = 0, corrupt_map_call = 0;
bool fail_read = false, fail_map = false, fail_nvs_open = false, fail_nvs_get = false;
bool fail_nvs_set = false, fail_nvs_commit = false, corrupt_readback = false;
bool nvs_was_committed = false;
esp_partition_t descriptor{};
void Reset() {
    assert(maps == 0 && handles == 0);
    assert(mapped_handles.empty());
    original = Bundle(); memory.assign(512 * 1024, 0xff);
    std::copy(original.begin(), original.end(), memory.begin());
    committed.reset(); pending.reset(); erases = writes = 0;
    map_calls = fail_map_call = corrupt_map_call = 0;
    fail_read = fail_map = fail_nvs_open = fail_nvs_get = fail_nvs_set = fail_nvs_commit = corrupt_readback = false;
    nvs_was_committed = false;
    descriptor = {}; descriptor.type = ESP_PARTITION_TYPE_DATA; descriptor.address = 0xA00000;
    descriptor.size = memory.size(); descriptor.erase_size = kSectorBytes; std::strcpy(descriptor.label, "assets");
}
void Range(const esp_partition_t* partition, size_t offset, size_t size) {
    assert(partition == &descriptor && offset <= memory.size() && size <= memory.size() - offset);
}
StageResult Stage(Store& store, const Bytes& data = Bundle()) {
    size_t cursor = 0;
    return store.Stage(Sign(Payload(data)), data.size(), [&](void* bytes, size_t maximum) {
        const auto size = std::min(data.size() - cursor, maximum);
        std::memcpy(bytes, data.data() + cursor, size); cursor += size; return static_cast<int>(size);
    }, [] { return false; }, [] { return true; });
}

struct Response {
    Bytes body;
    size_t declared = 0, cursor = 0;
    int status = 200, timeout = 0;
    unsigned created = 0, closed = 0;
    bool missing = false, open_ok = true, read_error = false, oversized = false, cancelled = false;
    int64_t now = 1000;
    std::string url;
    std::function<void()> on_open, on_read, on_status;
    void Body(const Bytes& value) { body = value; declared = body.size(); cursor = 0; }
    void Body(const std::string& value) { Body(Bytes(value.begin(), value.end())); }
};
class ResponseHttp final : public Http {
public:
    explicit ResponseHttp(Response& response) : response_(response) { ++response_.created; }
    ~ResponseHttp() override { assert(closed_); }
    void SetTimeout(int value) override { response_.timeout = value; }
    void SetHeader(const std::string&, const std::string&) override {}
    void SetContent(std::string&&) override { assert(false); }
    void SetKeepAlive(bool) override {}
    bool Open(const std::string& method, const std::string& url) override {
        assert(method == "GET" && response_.timeout == 15000);
        response_.url = url;
        if (response_.on_open) response_.on_open();
        return response_.open_ok;
    }
    void Close() override { assert(!closed_); closed_ = true; ++response_.closed; }
    int Read(char* bytes, size_t size) override {
        response_.now += 1000;
        if (response_.on_read) response_.on_read();
        if (response_.read_error) return -1;
        if (response_.oversized) return static_cast<int>(size + 1);
        const auto count = std::min({size, size_t(31), response_.body.size() - response_.cursor});
        if (!count) return 0;
        std::memcpy(bytes, response_.body.data() + response_.cursor, count);
        response_.cursor += count;
        return static_cast<int>(count);
    }
    int Write(const char*, size_t) override { assert(false); return -1; }
    int GetStatusCode() override {
        if (response_.on_status) response_.on_status();
        return response_.status;
    }
    std::string GetResponseHeader(const std::string&) const override { return {}; }
    size_t GetBodyLength() override { return response_.declared; }
    std::string ReadAll() override { assert(false); return {}; }
    int GetLastError() override { return 0; }
private:
    Response& response_;
    bool closed_ = false;
};
asset_update::Client Client(Store& store, Response& response) {
    return {store, [&response]() -> std::unique_ptr<Http> {
        if (response.missing) return {};
        return std::make_unique<ResponseHttp>(response);
    }, [&response] { return response.now; }};
}

void TestUpdateClient() {
    using Status = firmware_release::CheckStatus;
    for (int mode = 0; mode < 22; ++mode) {
        Reset(); PartitionStatus status;
        auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        if (mode != 19) store.Boot();
        if (mode == 20) { assert(Stage(store) == StageResult::kStaged); erases = writes = 0; }
        Response response; const auto envelope = Sign(Payload(Bundle())); response.Body(envelope);
        auto url = std::string("https://updates.example/models/latest");
        auto expected = Status::kInvalidRelease;
        switch (mode) {
            case 0: expected = Status::kAvailable; break;
            case 1: response.missing = true; expected = Status::kTransportError; break;
            case 2: response.open_ok = false; expected = Status::kTransportError; break;
            case 3: response.status = 302; expected = Status::kTransportError; break;
            case 4: response.declared = 0; break;
            case 5: response.declared = 4097; break;
            case 6: response.body.pop_back(); break;
            case 7: response.body.push_back('x'); break;
            case 8: response.body[response.body.size() / 2] ^= 1; break;
            case 9: response.Body(release_fixture::Seal(Payload(Bundle()))); break;
            case 10: response.Body(Sign(Change(Payload(Bundle()), "sequence", "0"))); break;
            case 11: response.Body(Sign(Change(Payload(Bundle()), "model", "other-watch"))); break;
            case 12: response.on_open = [&] { response.cancelled = true; }; expected = Status::kCancelled; break;
            case 13: response.on_read = [&] { response.cancelled = true; }; expected = Status::kCancelled; break;
            case 14: response.read_error = true; expected = Status::kTransportError; break;
            case 15: response.oversized = true; break;
            case 16: response.Body(Bytes{}); response.status = 204; expected = Status::kNoUpdate; break;
            case 17: response.declared = 0; response.status = 204; expected = Status::kTransportError; break;
            case 18: url = "http://updates.example/latest"; expected = Status::kNotConfigured; break;
            case 21: response.on_status = [&] { response.cancelled = true; }; expected = Status::kCancelled; break;
            default: expected = Status::kNotConfigured; break;
        }
        auto client = Client(store, response);
        const auto result = client.Check(url, [&] { return response.cancelled; });
        assert(result.status == expected);
        if (mode == 0) assert(result.release && result.envelope == envelope && result.release->Sequence() == 8);
        else assert(!result.release && result.envelope.empty());
        assert(response.created == response.closed && erases == 0 && writes == 0);
        if (mode >= 18 && mode <= 20) assert(response.created == 0);
    }
    for (int mode = 0; mode < 20; ++mode) {
        Reset(); PartitionStatus status;
        auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        store.Boot();
        Response response; auto envelope = Sign(Payload(Bundle())); response.Body(Bundle());
        unsigned commits = 0, progress_calls = 0;
        StageResult expected = StageResult::kIoError;
        switch (mode) {
            case 0: expected = StageResult::kStaged; break;
            case 1: response.missing = true; break;
            case 2: response.open_ok = false; break;
            case 3: response.status = 302; break;
            case 4: ++response.declared; expected = StageResult::kRejected; break;
            case 5: response.body.pop_back(); break;
            case 6: response.body.push_back(0); expected = StageResult::kInvalidImage; break;
            case 7: response.body.back() ^= 1; expected = StageResult::kInvalidImage; break;
            case 8: response.on_open = [&] { response.cancelled = true; }; expected = StageResult::kCancelled; break;
            case 9: response.on_read = [&] { response.cancelled = true; }; expected = StageResult::kCancelled; break;
            case 10: expected = StageResult::kCancelled; break;
            case 11: expected = StageResult::kStaged; break;
            case 12: fail_nvs_commit = true; expected = StageResult::kPersistenceUncertain; break;
            case 13: envelope = "invalid"; expected = StageResult::kRejected; break;
            case 14: response.cancelled = true; expected = StageResult::kCancelled; break;
            case 15: response.read_error = true; break;
            case 16: response.oversized = true; break;
            case 17: expected = StageResult::kCancelled; break;
            case 18: response.on_read = [&] { response.now -= 3000; }; expected = StageResult::kStaged; break;
            case 19: response.on_status = [&] { response.cancelled = true; }; expected = StageResult::kCancelled; break;
        }
        auto client = Client(store, response);
        const auto result = client.Install(envelope, [&](int percent, size_t) {
            assert(percent >= 0 && percent <= 99); ++progress_calls;
            if (mode == 17) response.cancelled = true;
        }, [&] { return response.cancelled; }, [&] {
            ++commits;
            if (mode == 11) response.cancelled = true;
            return mode != 10;
        });
        assert(result == expected && response.created == response.closed);
        assert(std::equal(original.begin(), original.end(), memory.begin()));
        if (mode != 1 && mode != 13 && mode != 14) assert(response.url == "https://updates.example/assets.bin");
        if (mode == 13 || mode == 14) assert(response.created == 0 && erases == 0);
        if (expected == StageResult::kStaged) {
            assert(commits == 1 && committed);
            Store reboot(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
            assert(reboot.Boot().trial);
        } else assert(!committed);
        if (mode == 0) assert(progress_calls > 0);
    }
    std::cout << "PASS: real asset HTTP client validates manifests, exact streams, cancellation, progress and journal publication\n";

    for (int mode = 0; mode < 3; ++mode) {
        Reset(); PartitionStatus status;
        auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        store.Boot(); Response response;
        const auto envelope = Sign(Payload(Bundle()));
        response.on_open = [&] {
            if (response.url == "https://updates.example/assets.bin") response.Body(Bundle());
            else response.Body(envelope);
        };
        auto client = Client(store, response);
        unsigned acquired = 0, released = 0, pins = 0;
        watch_update::Controller::Hooks hooks;
        hooks.target = mode == 1 ? watch_update::Target::kFirmware : watch_update::Target::kModels;
        hooks.check = [&](const auto& cancelled) -> watch_update::CheckResult {
            if (mode == 2) {
                const auto firmware_envelope = release_fixture::Seal(release_fixture::Payload(std::string(5000, 'x')));
                return firmware_release::CheckResult{Status::kAvailable,
                    firmware_release::Verify(firmware_envelope, "test-watch", 7, PublicKey()), firmware_envelope};
            }
            return client.Check("https://updates.example/models/latest", cancelled);
        };
        hooks.install = [&](const auto& selected, const auto& progress, const auto& cancelled, const auto& commit) {
            assert(selected == envelope);
            return firmware_release::InstallResult{
                client.Install(selected, progress, cancelled, commit) == StageResult::kStaged, true};
        };
        hooks.acquire = [&](auto) { ++acquired; return true; };
        hooks.release = [&](auto, bool safe) { assert(safe); ++released; return true; };
        hooks.now_ms = [] { return int64_t(1000); };
        hooks.pin_commit = [&] { ++pins; return true; };
        watch_update::Controller controller(std::move(hooks));
        const auto finish = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (controller.Read().active) {
                assert(std::chrono::steady_clock::now() < deadline);
                controller.Poll(); std::this_thread::yield();
            }
        };
        assert(controller.Check()); finish();
        const auto checked = controller.Read();
        if (mode) {
            assert(checked.phase == watch_update::Phase::kCheckFailed && !controller.Confirm(checked.attempt, 8));
            assert(pins == 0 && erases == 0);
        } else {
            assert(checked.target == watch_update::Target::kModels && checked.phase == watch_update::Phase::kAvailable);
            assert(controller.Confirm(checked.attempt, checked.sequence)); finish();
            const auto installed = controller.Read();
            assert(installed.phase == watch_update::Phase::kRebootRequired && installed.progress == 100);
            assert(installed.boot_selected && pins == 1 && committed);
        }
        assert(acquired == released && response.created == response.closed);
    }
    std::cout << "PASS: one update controller accepts typed asset releases and rejects cross-target confirmation\n";
}

void TestMappedUpdateOwnership() {
    for (int mode = 0; mode < 3; ++mode) {
        Reset();
        BootConfig config{kCompatibility, PublicKey()};
        {
            asset_storage::Mapping mapping;
            assert(mapping.Open(&descriptor, &config) && mapping.UpdatesReady());
            assert(mapping.Version().empty());
            mapping.Pin();
            const auto* original_pointer = mapping.Data();
            Response response; const auto envelope = Sign(Payload(Bundle())); response.Body(envelope);
            const auto acquire = [&] {
                return mapping.BeginUpdate([&]() -> std::unique_ptr<Http> { return std::make_unique<ResponseHttp>(response); },
                                           [&] { return response.now; });
            };
            auto session = acquire(); assert(session && !mapping.UpdatesReady() && !acquire());
            std::atomic<bool> entered{false}, proceed{false};
            response.on_read = [&] {
                entered.store(true);
                while (!proceed.load()) std::this_thread::yield();
            };
            asset_update::CheckResult checked;
            std::thread worker([&] { checked = session->Check("https://updates.example/latest", [] { return false; }); });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!entered.load()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
            assert(!mapping.UpdatesReady() && !mapping.Faulted() && !mapping.TrialPending());
            assert(mapping.Data() == original_pointer && !mapping.BeginTrialHealth(true, 0, 0));
            assert(!mapping.PollTrialHealth(1000, true, 1));
            assert(session->Check("https://updates.example/latest", [] { return false; }).status ==
                   firmware_release::CheckStatus::kTransportError);
            assert(session->Install(envelope, {}, [] { return false; }, [] { return true; }) == StageResult::kRejected);
            proceed.store(true); worker.join();
            assert(checked.release && checked.envelope == envelope && !mapping.UpdatesReady());
            session.reset(); assert(mapping.UpdatesReady() && !committed);
            response.on_read = {};
            response.Body(Bundle());
            if (mode == 1) response.on_read = [&] { response.cancelled = true; };
            fail_nvs_commit = mode == 2;
            session = acquire(); assert(session);
            StageResult installed = StageResult::kRejected;
            std::thread install([&] {
                installed = session->Install(checked.envelope, {}, [&] { return response.cancelled; }, [] { return true; });
            });
            install.join();
            assert(installed == (mode == 0 ? StageResult::kStaged : mode == 1 ? StageResult::kCancelled : StageResult::kPersistenceUncertain));
            assert(!mapping.UpdatesReady() && !mapping.Faulted());
            session.reset();
            assert(mapping.UpdatesReady() == (mode == 1));
            assert(mapping.Faulted() == (mode == 2));
            assert(mapping.Data() == original_pointer && mapping.Selected().slot == 0 && maps == 1);
            assert(mapping.Version().empty());
            assert(std::equal(original.begin(), original.end(), static_cast<const unsigned char*>(mapping.Data())));
            assert(response.created == response.closed && handles == 0);
        }
        esp_partition_munmap(0);
        fail_nvs_commit = false;
        asset_storage::Mapping reboot;
        assert(reboot.Open(&descriptor, &config) && reboot.TrialPending() == (mode == 0));
        assert(reboot.Version() == (mode == 0 ? "1.0.0" : ""));
    }
    std::cout << "PASS: mapped asset state transfers exclusively to one worker and returns only after join\n";
}
}  // namespace

extern "C" esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset, void* bytes, size_t size) {
    Range(partition, offset, size); if (fail_read) return ESP_FAIL;
    std::memcpy(bytes, memory.data() + offset, size); return ESP_OK;
}
extern "C" esp_err_t esp_partition_write(const esp_partition_t* partition, size_t offset, const void* bytes, size_t size) {
    Range(partition, offset, size); assert(offset >= kMapAlignment); ++writes;
    if (partition->encrypted) assert(offset % 16 == 0 && size % 16 == 0);
    const auto* source = static_cast<const unsigned char*>(bytes);
    for (size_t i = 0; i < size; ++i) { assert((memory[offset + i] & source[i]) == source[i]); memory[offset + i] = source[i]; }
    return ESP_OK;
}
extern "C" esp_err_t esp_partition_erase_range(const esp_partition_t* partition, size_t offset, size_t size) {
    Range(partition, offset, size); assert(offset >= kMapAlignment && offset % kSectorBytes == 0 && size % kSectorBytes == 0);
    ++erases; std::fill_n(memory.begin() + offset, size, 0xff); return ESP_OK;
}
extern "C" esp_err_t esp_partition_mmap(const esp_partition_t* partition, size_t offset, size_t size,
                                        esp_partition_mmap_memory_t kind, const void** bytes,
                                        esp_partition_mmap_handle_t* handle) {
    Range(partition, offset, size); assert(offset % kMapAlignment == 0 && kind == ESP_PARTITION_MMAP_DATA);
    if (fail_map || ++map_calls == fail_map_call) return ESP_FAIL;
    if (map_calls == corrupt_map_call) {
        const std::string name = "wn9_data";
        auto field = std::search(memory.begin() + offset, memory.begin() + offset + size, name.begin(), name.end());
        assert(field != memory.begin() + offset + size);
        ++field[4]; --memory[offset + size - 1]; // Preserve the legacy additive checksum.
    }
    // Zero is deliberately a successful handle: cleanup must use the success flag.
    *handle = 0;
    while (mapped_handles.count(*handle)) ++*handle;
    mapped_handles.insert(*handle); ++maps; *bytes = memory.data() + offset; return ESP_OK;
}
extern "C" void esp_partition_munmap(esp_partition_mmap_handle_t handle) {
    assert(maps && mapped_handles.erase(handle) == 1); --maps;
}
extern "C" uint32_t esp_partition_get_main_flash_sector_size() { return kSectorBytes; }
extern "C" esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    assert(std::strcmp(name, "asset_update") == 0);
    if (fail_nvs_open) return ESP_FAIL;
    if (mode == NVS_READONLY && !committed) return ESP_ERR_NVS_NOT_FOUND;
    assert(handles == 0); ++handles; *handle = 17; return ESP_OK;
}
extern "C" void nvs_close(nvs_handle_t handle) { assert(handle == 17 && handles == 1); --handles; pending.reset(); }
extern "C" esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* value, size_t* size) {
    assert(handle == 17 && handles == 1 && std::strcmp(key, "journal_v1") == 0);
    if (fail_nvs_get) return ESP_FAIL;
    if (!committed) return ESP_ERR_NVS_NOT_FOUND;
    if (!value) { *size = committed->size(); return ESP_OK; }
    if (*size < committed->size()) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(value, committed->data(), committed->size()); *size = committed->size();
    if (corrupt_readback && nvs_was_committed) static_cast<unsigned char*>(value)[30] ^= 1;
    return ESP_OK;
}
extern "C" esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t size) {
    assert(handle == 17 && handles == 1 && std::strcmp(key, "journal_v1") == 0 && size == kJournalBytes);
    if (fail_nvs_set) return ESP_FAIL;
    const auto* bytes = static_cast<const unsigned char*>(value); pending = Bytes(bytes, bytes + size); return ESP_OK;
}
extern "C" esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 17 && handles == 1 && pending);
    if (fail_nvs_commit) return ESP_FAIL;
    committed = pending; nvs_was_committed = true; return ESP_OK;
}

int main() {
    assert(sodium_init() >= 0);
    Reset();
    PartitionStatus status;
    assert(!Partition::Open(nullptr, status) && status == PartitionStatus::kUnsupported);
    descriptor.type = ESP_PARTITION_TYPE_APP;
    assert(!Partition::Open(&descriptor, status) && status == PartitionStatus::kUnsupported);
    Reset(); descriptor.readonly = true;
    assert(!Partition::Open(&descriptor, status) && status == PartitionStatus::kUnsupported);
    Reset(); descriptor.erase_size = 8192;
    assert(!Partition::Open(&descriptor, status) && status == PartitionStatus::kUnsupported);
    Reset(); fail_read = true;
    assert(!Partition::Open(&descriptor, status) && status == PartitionStatus::kIoError);
    Reset(); fail_map = true;
    assert(!Partition::Open(&descriptor, status) && status == PartitionStatus::kIoError && maps == 0);
    Reset(); memory[100] ^= 1;
    assert(!Partition::Open(&descriptor, status) && status == PartitionStatus::kInvalidFactory && maps == 0);
    Reset(); descriptor.size = 65536;
    assert(!Partition::Open(&descriptor, status) && status == PartitionStatus::kInsufficientSpace && maps == 0);
    assert(erases == 0 && writes == 0);
    Reset();
    {
        auto backend = Partition::Open(&descriptor, status);
        assert(backend && status == PartitionStatus::kReady && maps == 0);
        auto hooks = backend->MakeHooks();
        unsigned char byte = 0;
        assert(!hooks.write(0, &byte, 1) && !hooks.erase(0, kSectorBytes));
        assert(!hooks.write(backend->Geometry().Offset(2) - 1, &byte, 2));
        assert(!hooks.erase(backend->Geometry().Offset(1) + 1, kSectorBytes));
        assert(!hooks.read(UINT32_MAX, &byte, 1) && !hooks.write(UINT32_MAX, &byte, 1));
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), hooks);
        assert(store.Boot().slot == 0 && Stage(store) == StageResult::kStaged);
        assert(maps == 0 && handles == 0);
        Store trial(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), hooks);
        assert(trial.Boot().trial && trial.ConfirmTrial(8));
        Store confirmed(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), hooks);
        assert(confirmed.Boot().slot == 1 && !confirmed.Selected().trial);
        assert(std::equal(original.begin(), original.end(), memory.begin()));
        assert(maps == 0 && handles == 0);
    }
    for (int mode = 0; mode < 4; ++mode) {
        Reset(); auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        store.Boot();
        if (mode == 0) fail_nvs_open = true;
        if (mode == 1) fail_nvs_set = true;
        if (mode == 2) fail_nvs_commit = true;
        if (mode == 3) corrupt_readback = true;
        assert(Stage(store) == StageResult::kPersistenceUncertain && store.Faulted());
        assert(maps == 0 && handles == 0);
        assert(std::equal(original.begin(), original.end(), memory.begin()));
    }
    {
        Reset(); descriptor.encrypted = true;
        auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        store.Boot();
        assert(Stage(store) == StageResult::kStaged);
        Store trial(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        assert(trial.Boot().trial && trial.ConfirmTrial(8));
        assert(maps == 0 && handles == 0);
    }
    for (int mode = 0; mode < 3; ++mode) {
        Reset(); auto backend = Partition::Open(&descriptor, status); assert(backend);
        committed = Bytes(kJournalBytes, 0);
        if (mode == 0) fail_nvs_get = true;
        if (mode == 1) committed->push_back(0);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        assert(store.Boot().slot == 0 && store.Faulted());
        assert(maps == 0 && handles == 0 && erases == 0 && writes == 0);
    }
    {
        Reset(); auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        store.Boot();
        const auto update = Bundle(R"({"version":1,"srmodels":"srmodels.bin"}   )");
        assert(Stage(store, update) == StageResult::kStaged);
        const auto before = committed;
        {
            asset_storage::Mapping legacy;
            assert(legacy.Open(&descriptor) && legacy.Selected().slot == 0 && legacy.Size() == original.size());
            assert(legacy.SupportsFirmware(0) && !legacy.SupportsFirmware(7));
            assert(!legacy.UpdatesReady() && !legacy.TrialPending() && maps == 1);
            assert(legacy.Data() == memory.data() && committed == before);
        }
        BootConfig config{kCompatibility, PublicKey()};
        assert(IsValidConfig(config));
        {
            auto invalid = config; invalid.public_key_hex.clear(); assert(!IsValidConfig(invalid));
            invalid = config; invalid.factory_firmware_min = 9; invalid.factory_firmware_max = 8;
            assert(!IsValidConfig(invalid));
            asset_storage::Mapping fallback;
            auto missing_key = config; missing_key.public_key_hex.clear();
            assert(fallback.Open(&descriptor, &missing_key) && fallback.Selected().slot == 0 && fallback.Faulted());
            assert(committed == before && !fallback.UpdatesReady());
        }
        {
            asset_storage::Mapping trial;
            assert(trial.Open(&descriptor, &config) && !trial.UpdatesReady() && trial.TrialPending());
            assert(!trial.SupportsFirmware(7));
            assert(trial.Selected().slot == 1 && trial.Size() == update.size() && maps == 1);
            assert(trial.Version() == "1.0.0");
            assert(trial.Data() == memory.data() + backend->Geometry().Offset(1));
            assert(std::memcmp(trial.Data(), update.data(), update.size()) == 0);
            const auto attempted = committed;
            assert(trial.Open(&descriptor, &config) && committed == attempted && maps == 1);
        }
        {
            asset_storage::Mapping rollback;
            assert(rollback.Open(&descriptor, &config) && !rollback.TrialPending());
            assert(rollback.Selected().slot == 0 && rollback.Data() == memory.data());
            assert(rollback.SupportsFirmware(0) && !rollback.SupportsFirmware(7));
            rollback.Pin();
        }
        assert(maps == 1); // Borrowed SDK pointers survive owner destruction until reboot.
        esp_partition_munmap(0); // Simulated reboot releases the global SDK mapping.
    }
    {
        Reset(); auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        store.Boot(); assert(Stage(store) == StageResult::kStaged);
        BootConfig config{kCompatibility, PublicKey()};
        map_calls = 0; fail_map_call = 3;
        {
            asset_storage::Mapping failed;
            assert(!failed.Open(&descriptor, &config) && failed.Faulted() && !failed.Data());
            assert(failed.TrialPending() && maps == 0);
            assert(!failed.Open(&descriptor, &config));
        }
        fail_map_call = 0;
        asset_storage::Mapping fallback;
        assert(fallback.Open(&descriptor, &config) && fallback.Selected().slot == 0);
    }
    {
        Reset(); descriptor.size = 65536;
        BootConfig config{kCompatibility, PublicKey()};
        asset_storage::Mapping factory;
        assert(factory.Open(&descriptor, &config) && factory.Selected().slot == 0 && !factory.UpdatesReady());
        assert(factory.StorageStatus() == PartitionStatus::kInsufficientSpace && !committed);
    }
    {
        Reset(); auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        store.Boot(); assert(Stage(store) == StageResult::kStaged);
        BootConfig config{kCompatibility, PublicKey()};
        map_calls = 0; corrupt_map_call = 3;
        asset_storage::Mapping changed;
        assert(!changed.Open(&descriptor, &config) && changed.Faulted() && !changed.Data() && maps == 0);
    }
    for (int mode = 0; mode < 9; ++mode) {
        Reset(); auto backend = Partition::Open(&descriptor, status); assert(backend);
        Store store(backend->Geometry(), backend->FactoryDigest(), kCompatibility, PublicKey(), backend->MakeHooks());
        store.Boot();
        const auto data = mode == 7 ? Bundle(R"({"version":1,"srmodels":"srmodels.bin","skin":{}})") : Bundle();
        assert(Stage(store, data) == StageResult::kStaged);
        BootConfig config{kCompatibility, PublicKey()};
        {
            asset_storage::Mapping trial;
            assert(trial.Open(&descriptor, &config));
            trial.Pin();
            const auto trial_journal = committed;
            if (mode == 7) {
                assert(!trial.BeginTrialHealth(true, 0, 0));
                assert(!trial.Faulted() && trial.TrialPending() && committed == trial_journal);
                assert(!trial.PollTrialHealth(60000, true, 100) && committed == trial_journal);
            } else if (mode == 1) {
                assert(!trial.BeginTrialHealth(false, 0, 0));
                assert(!trial.BeginTrialHealth(true, 0, 0) && trial.Faulted());
                assert(!trial.PollTrialHealth(60000, true, 100));
            } else if (mode == 8) {
                assert(trial.BeginTrialHealth(true, 0, 0));
                assert(trial.TrialState() == asset_storage::TrialStatus::kObserving);
                assert(trial.EndTrialHealth(true));
                assert(trial.TrialState() == asset_storage::TrialStatus::kFailed && trial.Faulted());
                assert(!trial.BeginTrialHealth(true, 1000, 1));
            } else {
                assert(trial.BeginTrialHealth(true, 0, 0));
                if (mode == 0) {
                    assert(trial.EndTrialHealth(false));
                    assert(trial.TrialState() == asset_storage::TrialStatus::kPending && !trial.Faulted());
                    assert(trial.BeginTrialHealth(true, 0, 0));
                }
                uint32_t feeds = 0;
                for (int i = 1; i <= 29; ++i) {
                    if (mode != 2) ++feeds;
                    assert(trial.PollTrialHealth(i * 1000, true, feeds));
                }
                // Repeated initialization must not restart or bypass the observation.
                assert(trial.BeginTrialHealth(true, 29000, feeds));
                assert(committed == trial_journal && trial.TrialPending());
                if (mode == 2) {
                    assert(trial.PollTrialHealth(30000, true, feeds));
                    assert(committed == trial_journal);
                } else if (mode == 3) {
                    fail_nvs_commit = true;
                    assert(!trial.PollTrialHealth(30000, true, ++feeds));
                    assert(trial.Faulted() && trial.TrialPending() && committed == trial_journal);
                    fail_nvs_commit = false;
                } else if (mode == 4 || mode == 5) {
                    const int64_t restart = mode == 4 ? 30000 : 32001;
                    assert(trial.PollTrialHealth(restart, mode != 4, ++feeds));
                    for (int i = 1; i <= 29; ++i)
                        assert(trial.PollTrialHealth(restart + i * 1000, true, ++feeds));
                    assert(committed == trial_journal);
                    assert(!trial.PollTrialHealth(restart + 30000, true, ++feeds));
                    assert(!trial.TrialPending() && !trial.Faulted());
                } else if (mode == 6) {
                    assert(!trial.PollTrialHealth(28000, true, ++feeds));
                    assert(trial.Faulted() && trial.TrialPending() && committed == trial_journal);
                } else {
                    assert(!trial.PollTrialHealth(30000, true, ++feeds));
                    assert(!trial.TrialPending() && !trial.Selected().trial && !trial.Faulted());
                }
            }
            assert(trial.Data() == memory.data() + backend->Geometry().Offset(1) && maps == 1);
        }
        esp_partition_munmap(0);
        asset_storage::Mapping next;
        assert(next.Open(&descriptor, &config) && !next.TrialPending());
        assert(next.Selected().slot == ((mode == 0 || mode == 4 || mode == 5) ? 1 : 0));
        if (next.Selected().slot) {
            assert(next.SupportsFirmware(6) && next.SupportsFirmware(9));
            assert(!next.SupportsFirmware(5) && !next.SupportsFirmware(10));
        } else {
            assert(next.SupportsFirmware(0) && !next.SupportsFirmware(7));
        }
    }
    assert(maps == 0 && handles == 0);
    TestUpdateClient();
    TestMappedUpdateOwnership();
    std::cout << "PASS: trial health requires prepared pinned models, sustained wake input and durable confirmation\n";
    std::cout << "PASS: selected-bank boot mapping, factory fallback, trial failure and pinned model lifetime\n";
    std::cout << "PASS: actual partition/NVS adapter with SDK headers, bounded mapping, factory protection and verified commits\n";
}
