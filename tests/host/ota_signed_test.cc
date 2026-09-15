#include "ota.h"
#include "firmware_release_fixture.h"
#include "firmware_release_identity.h"
#include "watch/watch_firmware_update.h"
#include "audio/audio_session_coordinator.h"
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <chrono>

namespace {
struct Fixture {
    esp_partition_t partition{32768};
    std::string image, manifest, written;
    size_t fragment = 7;
    int opened = 0, closed = 0, begins = 0, aborted = 0, ended = 0, selected = 0;
    int manifest_length_delta = 0, image_length_delta = 0;
    int manifest_status = 200, manifest_reads = 0, fail_manifest_read = -1, live_http = 0;
    bool active = false, native_valid = true, abort_failed = false;
    std::function<void(const char*)> event;
} f;
void Reset() {
    assert(!f.active && !f.live_http);
    f = {};
    f.image.assign(5000, '\0');
    esp_app_desc_t descriptor{};
    descriptor.magic_word = ESP_APP_DESC_MAGIC_WORD;
    std::strcpy(descriptor.version, "3.0.0");
    std::memcpy(f.image.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
                &descriptor, sizeof(descriptor));
    const firmware_release::ImageIdentity identity{"AUDOTA1", 8, "test-watch"};
    std::memcpy(f.image.data() + 288, &identity, sizeof(identity));
    f.manifest = release_fixture::Seal(release_fixture::Payload(f.image));
    Board::GetInstance().profile = BoardRuntimeProfile::kWatchAssistantRecorder;
}
class Response : public Http {
public:
    Response() { assert(++f.live_http == 1); }
    ~Response() override { --f.live_http; }
    void SetTimeout(int ms) override { assert(ms == 15000); }
    void SetHeader(const std::string&, const std::string&) override { assert(false); }
    void SetContent(std::string&&) override { assert(false); }
    void SetKeepAlive(bool) override {}
    bool Open(const std::string& method, const std::string& url) override {
        assert(method == "GET"); ++f.opened;
        manifest_ = url == "https://updates.example/release.json";
        manifest_ = manifest_ || url == "https://updates.example/v1/firmware/releases/test-watch/after/7";
        assert(manifest_ || url == "https://updates.example/firmware.bin");
        return true;
    }
    void Close() override { ++f.closed; }
    int Read(char* out, size_t maximum) override {
        if (manifest_ && f.manifest_reads++ == f.fail_manifest_read) return -1;
        const auto& data = manifest_ ? f.manifest : f.image;
        const auto size = std::min({maximum, f.fragment, data.size() - offset_});
        if (size) std::memcpy(out, data.data() + offset_, size);
        offset_ += size;
        if (!manifest_ && f.event) f.event("read");
        return static_cast<int>(size);
    }
    int Write(const char*, size_t) override { assert(false); return -1; }
    int GetStatusCode() override { return manifest_ ? f.manifest_status : 200; }
    std::string GetResponseHeader(const std::string&) const override { return {}; }
    size_t GetBodyLength() override {
        return manifest_ ? f.manifest.size() + f.manifest_length_delta : f.image.size() + f.image_length_delta;
    }
    std::string ReadAll() override { assert(false); return {}; }
    int GetLastError() override { return 0; }
private:
    bool manifest_ = false;
    size_t offset_ = 0;
};
bool Run() {
    bool complete = false;
    const auto ok = Ota::UpgradeSigned("https://updates.example/release.json", [&](int progress, size_t) {
        if (progress == 100) { assert(f.selected == 1); complete = true; }
    });
    assert(ok == complete && !f.active && !f.live_http && f.opened == f.closed);
    if (!ok) assert(f.selected == 0);
    return ok;
}
}

std::unique_ptr<Http> OtaTestNetwork::CreateHttp(int) { return std::make_unique<Response>(); }
OtaTestNetwork* Board::GetNetwork() { static OtaTestNetwork network; return &network; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return &f.partition; }
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t* handle) {
    assert(!f.active); f.active = true; *handle = 1; ++f.begins;
    if (f.event) f.event("begin");
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t, const void* bytes, size_t size) {
    assert(f.active); f.written.append(static_cast<const char*>(bytes), size);
    if (f.event) f.event("write");
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t) {
    assert(f.active && f.written == f.image); f.active = false; ++f.ended;
    if (f.event) f.event("end");
    return f.native_valid ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_ota_abort(esp_ota_handle_t) {
    assert(f.active); f.active = false; ++f.aborted;
    return f.abort_failed ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) {
    assert(f.ended == 1 && f.native_valid && !f.active); ++f.selected;
    if (f.event) f.event("select");
    return ESP_OK;
}
void* heap_caps_malloc(size_t size, unsigned) { return std::malloc(size); }
void heap_caps_free(void* data) { std::free(data); }
int64_t esp_timer_get_time() { static int64_t now = 0; return now += 10000; }
const char* esp_err_to_name(esp_err_t) { return "test error"; }

int main(int argc, char** argv) {
    using firmware_release::CheckStatus;
    assert(sodium_init() >= 0);
    for (size_t fragment : {1, 7, 512, 4096}) {
        Reset(); f.fragment = fragment; assert(Run());
    }
    Reset(); assert(!Ota::Upgrade("https://updates.example/firmware.bin", {})); assert(f.opened == 0);
    Reset(); assert(!Ota::UpgradeSigned("http://updates.example/release.json", {})); assert(f.opened == 0);
    for (int status : {301, 404, 503}) {
        Reset(); f.manifest_status = status; assert(!Run() && !f.begins && f.opened == 1);
    }
    for (int read : {0, 1, 20}) {
        Reset(); f.fail_manifest_read = read; assert(!Run() && !f.begins && f.opened == 1);
    }
    Reset(); f.manifest_length_delta = -static_cast<int>(f.manifest.size()); assert(!Run() && !f.begins);
    Reset(); f.manifest_length_delta = 4097; assert(!Run() && !f.begins);
    for (int delta : {-1, 1}) {
        Reset(); f.manifest_length_delta = delta; assert(!Run() && !f.begins && f.opened == 1);
        Reset(); f.image_length_delta = delta; assert(!Run() && !f.begins);
    }
    Reset(); f.manifest[25] ^= 1; assert(!Run() && !f.begins && f.opened == 1);
    Reset(); f.manifest = release_fixture::Seal(release_fixture::Change(release_fixture::Payload(f.image), "sequence", "7"));
    assert(!Run() && !f.begins && f.opened == 1);
    Reset(); f.manifest = release_fixture::Seal(release_fixture::Change(release_fixture::Payload(f.image), "model", "other"));
    assert(!Run() && !f.begins && f.opened == 1);
    Reset(); f.image.back() ^= 1; assert(!Run() && f.aborted == 1 && f.ended == 0);
    Reset(); f.image[48] = '4';
    f.manifest = release_fixture::Seal(release_fixture::Payload(f.image));
    assert(!Run() && !f.begins);
    Reset(); f.native_valid = false; assert(!Run() && f.ended == 1 && f.aborted == 0);
    for (size_t offset : {288, 296, 300}) {
        Reset(); f.image[offset] ^= 1;
        f.manifest = release_fixture::Seal(release_fixture::Payload(f.image));
        assert(!Run() && !f.begins);
    }
    Reset();
    const auto found = Ota::CheckSignedRelease();
    assert(found.status == CheckStatus::kAvailable && found.release && found.envelope == f.manifest);
    assert(f.opened == 1 && f.closed == 1 && !f.begins && !f.live_http);
    f.manifest = "invalid latest response";
    assert(Ota::UpgradeSignedEnvelope(found.envelope, {}));
    assert(f.opened == 2 && f.closed == 2 && f.selected == 1);
    Reset(); f.manifest_status = 204; f.manifest_length_delta = -static_cast<int>(f.manifest.size());
    const auto current = Ota::CheckSignedRelease();
    assert(current.status == CheckStatus::kNoUpdate && !current.release && current.envelope.empty() && !f.begins);
    for (int status : {404, 503}) {
        Reset(); f.manifest_status = status;
        assert(Ota::CheckSignedRelease().status == CheckStatus::kTransportError && !f.begins);
    }
    Reset(); f.manifest[25] ^= 1;
    assert(Ota::CheckSignedRelease().status == CheckStatus::kInvalidRelease && !f.begins);
    assert(!Ota::UpgradeSignedEnvelope(f.manifest, {}) && f.opened == 1 && !f.begins);
    for (int after : {0, 1, 3, 30}) {
        Reset();
        int calls = 0;
        const auto cancelled = Ota::CheckSignedRelease([&] { return calls++ >= after; });
        assert(cancelled.status == CheckStatus::kCancelled && !cancelled.release && cancelled.envelope.empty());
        assert(!f.begins && !f.live_http && f.closed == f.opened);
    }
    for (const std::string stage : {"read", "begin", "write", "end", "select"}) {
        Reset();
        bool cancel = false;
        bool complete = false;
        f.event = [&](const char* event) { if (stage == event) cancel = true; };
        const bool installed = Ota::UpgradeSignedEnvelope(f.manifest,
            [&](int value, size_t) { if (value == 100) complete = true; }, [&] { return cancel; });
        assert(cancel && installed == (stage == "select") && complete == installed);
        assert(!f.active && !f.live_http && f.closed == f.opened);
        assert(f.selected == (installed ? 1 : 0));
        if (stage == "begin" || stage == "write") assert(f.aborted == 1 && !f.ended);
        if (stage == "end") assert(!f.aborted && f.ended == 1);
    }
    Reset();
    assert(!Ota::UpgradeSignedEnvelope(f.manifest, {}, {}, [] { return false; }));
    assert(f.ended == 1 && !f.selected && !f.aborted);
    Reset();
    bool late_cancel = false;
    assert(Ota::UpgradeSignedEnvelope(f.manifest, {}, [&] { return late_cancel; }, [&] {
        late_cancel = true;
        return true;
    }));
    assert(f.selected == 1 && !f.aborted);
    for (bool deny_commit : {false, true}) {
        Reset();
        int acquired = 0, released = 0;
        watch_update::Controller* owner = nullptr;
        watch_update::Controller controller({
            [](const auto& cancelled) { return Ota::CheckSignedRelease(cancelled); },
            [&](const auto& envelope, const auto& progress, const auto& cancelled, const auto& before_commit) {
                return Ota::InstallSignedEnvelope(envelope, progress, cancelled, [&] {
                    if (deny_commit) assert(owner->Cancel(owner->Read().attempt));
                    return before_commit();
                });
            },
            [&](auto) { ++acquired; return true; },
            [&](auto, bool io_safe) { ++released; assert(io_safe && !f.active && !f.live_http); return true; },
            [] { return int64_t{1000}; },
            [] { return true; }
        });
        owner = &controller;
        const auto finish = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (controller.Read().active) {
                assert(std::chrono::steady_clock::now() < deadline);
                controller.Poll();
                std::this_thread::yield();
            }
        };
        assert(controller.Check());
        finish();
        assert(controller.Read().phase == watch_update::Phase::kAvailable);
        assert(controller.Confirm(controller.Read().attempt, 8));
        finish();
        assert(controller.Read().phase == (deny_commit ? watch_update::Phase::kCancelled : watch_update::Phase::kRebootRequired));
        assert(f.selected == (deny_commit ? 0 : 1) && acquired == 2 && released == 2);
    }
    for (const auto stage : {"write", "end", "select"}) {
        Reset();
        AudioSessionCoordinator coordinator;
        uint64_t generation = 0;
        AudioSessionTransition recording;
        bool attempted = false;
        f.event = [&](const char* event) {
            if (attempted || std::strcmp(stage, event) != 0) return;
            attempted = true;
            recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
        };
        watch_update::Controller controller({
            [](const auto& cancelled) { return Ota::CheckSignedRelease(cancelled); },
            [](const auto& envelope, const auto& progress, const auto& cancelled, const auto& commit) {
                return Ota::InstallSignedEnvelope(envelope, progress, cancelled, commit);
            },
            [&](auto) {
                const auto lease = coordinator.Begin(AudioSessionMode::kFirmwareUpdate);
                generation = lease.generation;
                return coordinator.Complete(lease);
            },
            [&](auto, bool safe) {
                assert(safe && !f.active && !f.live_http);
                if (!coordinator.IsCurrent(generation)) {
                    assert(!coordinator.ReleaseFirmwareUpdate(generation, safe));
                    return safe;
                }
                return coordinator.ReleaseFirmwareUpdate(generation, safe);
            },
            [] { return int64_t{1000}; },
            [&] { return coordinator.BeginFirmwareCommit(generation); }
        });
        const auto finish = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (controller.Read().active) {
                assert(std::chrono::steady_clock::now() < deadline);
                controller.Poll();
                std::this_thread::yield();
            }
        };
        assert(controller.Check());
        finish();
        assert(controller.Confirm(controller.Read().attempt, 8));
        finish();
        assert(attempted);
        if (std::strcmp(stage, "select") == 0) {
            assert(!recording && f.selected == 1 && !controller.StopWithin(0));
            assert(controller.Read().phase == watch_update::Phase::kRebootRequired);
            assert(coordinator.Mode() == AudioSessionMode::kFirmwareCommit);
        } else {
            // No cancellation notification is needed to reject a stale commit.
            assert(recording && f.selected == 0 && controller.StopWithin(0));
            assert(!controller.Read().commit_started && !controller.Read().boot_selected);
            assert(coordinator.Complete(recording));
        }
    }
    Reset();
    f.abort_failed = true;
    bool cancel_write = false;
    f.event = [&](const char* event) { if (std::string(event) == "write") cancel_write = true; };
    const auto uncertain_cleanup = Ota::InstallSignedEnvelope(f.manifest, {}, [&] { return cancel_write; });
    assert(!uncertain_cleanup.installed && !uncertain_cleanup.resources_released && f.aborted == 1 && !f.selected);
    if (argc == 3) {
        Reset();
        std::ifstream manifest(argv[1]), image(argv[2], std::ios::binary);
        assert(manifest && image);
        f.manifest.assign(std::istreambuf_iterator<char>(manifest), {});
        f.image.assign(std::istreambuf_iterator<char>(image), {});
        const auto discovered = Ota::CheckSignedRelease();
        assert(discovered.status == CheckStatus::kAvailable && discovered.release && !f.begins);
        f.manifest = "changed after approval";
        assert(Ota::UpgradeSignedEnvelope(discovered.envelope, {}) && f.selected == 1 && !f.active && !f.live_http);
        std::cout << "PASS: Python-published release through discovery and pinned signed OTA entrypoints\n";
    }
    std::cout << "PASS: signed OTA, unsigned denial, metadata binding and stream digest rejection\n";
}
