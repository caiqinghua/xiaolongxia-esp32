#include "ap_test_sdk.h"
#include "storage/temporary_ap_transfer_service.h"
#include "watch/watch_wifi_gate.h"
#include "system/battery_telemetry_service.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

namespace ap_test {
std::function<int()> http_stop = [] { return ESP_OK; };
std::function<void()> metadata_read = [] {};
std::function<void()> wifi_start = [] {}, socket_shutdown = [] {};
std::function<void()> timer_start = [] {};
std::atomic<int> stop_error{0}, deinit_calls{0}, stop_calls{0}, tasks{0};
std::atomic<int> http_start_error{0};
std::atomic<int> stopped_events{0};
std::atomic<bool> task_failure{false}, powered{false}, manager_initialized{true};
httpd_uri_t metadata_handler{};
TestTimer timer;
std::atomic<int64_t> now{1};
std::mutex workers_mutex;
std::vector<std::thread> workers;
void JoinTasks() {
    for (;;) {
        std::vector<std::thread> batch;
        { std::lock_guard<std::mutex> lock(workers_mutex); batch.swap(workers); }
        if (batch.empty()) return;
        for (auto& thread : batch) thread.join();
    }
}
}
using namespace ap_test;
int64_t esp_timer_get_time() { return now.load(); }
int esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
    timer = {false, args->callback, args->arg}; *out = &timer; return ESP_OK;
}
int esp_timer_stop(esp_timer_handle_t t) { t->active = false; return ESP_OK; }
bool esp_timer_is_active(esp_timer_handle_t t) { return t->active; }
int esp_timer_start_once(esp_timer_handle_t t, uint64_t) { timer_start(); t->active = true; return ESP_OK; }
int esp_netif_init() { return ESP_OK; }
esp_netif_t* esp_netif_create_default_wifi_ap() { return new esp_netif_t; }
void esp_netif_destroy_default_wifi(esp_netif_t* p) { delete p; }
int esp_netif_dhcps_stop(esp_netif_t*) { return ESP_OK; }
int esp_netif_dhcps_start(esp_netif_t*) { return ESP_OK; }
int esp_netif_set_ip_info(esp_netif_t*, esp_netif_ip_info_t*) { return ESP_OK; }
int esp_event_loop_create_default() { return ESP_OK; }
int esp_wifi_init(wifi_init_config_t*) { return ESP_OK; }
int esp_wifi_stop() { return stop_error.load(); }
int esp_wifi_deinit() { ++deinit_calls; return ESP_OK; }
int esp_wifi_set_mode(int) { return ESP_OK; }
int esp_wifi_set_config(int, wifi_config_t*) { return ESP_OK; }
int esp_wifi_set_ps(int) { return ESP_OK; }
int esp_wifi_start() { wifi_start(); return ESP_OK; }
int esp_wifi_ap_get_sta_list(wifi_sta_list_t* p) { p->num = 0; return ESP_OK; }
int esp_efuse_mac_get_default(uint8_t* p) { std::memset(p, 1, 6); return ESP_OK; }
uint32_t esp_random() { static uint32_t value = 0; return ++value; }
size_t heap_caps_get_free_size(int) { return 1024 * 1024; }
size_t heap_caps_get_largest_free_block(int) { return 1024 * 1024; }
size_t heap_caps_get_minimum_free_size(int) { return 1024 * 1024; }
void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }
void heap_caps_free(void* p) { std::free(p); }
bool httpd_uri_match_wildcard(const char*, const char*, size_t) { return true; }
int httpd_start(httpd_handle_t* out, httpd_config_t*) {
    if (http_start_error) return http_start_error.load();
    *out = &metadata_handler; return ESP_OK;
}
int httpd_stop(httpd_handle_t h) { assert(h == &metadata_handler); ++stop_calls; return http_stop(); }
int httpd_register_uri_handler(httpd_handle_t, httpd_uri_t* uri) {
    if (std::strcmp(uri->uri, "/metadata*") == 0) metadata_handler = *uri;
    return ESP_OK;
}
int httpd_req_get_url_query_str(httpd_req_t* r, char* out, size_t n) { std::snprintf(out, n, "%s", r->token.c_str()); return ESP_OK; }
int httpd_query_key_value(const char* q, const char* key, char* out, size_t n) {
    std::snprintf(out, n, "%s", std::strcmp(key, "token") == 0 ? q : "recording"); return ESP_OK;
}
int httpd_resp_send_err(httpd_req_t* r, int error, const char*) { r->error = error; return ESP_OK; }
int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
int httpd_resp_send_chunk(httpd_req_t*, const char*, size_t) { return ESP_OK; }
int httpd_send(httpd_req_t*, const char*, size_t n) { return static_cast<int>(n); }
int httpd_req_to_sockfd(httpd_req_t*) { return 42; }
int ap_test_shutdown(int socket, int mode) { assert(socket == 42 && mode == 2); socket_shutdown(); return 0; }
QueueHandle_t xQueueCreate(size_t, size_t) { return nullptr; }
int xQueueReceive(QueueHandle_t, void*, TickType_t) { return 0; }
int xQueueSend(QueueHandle_t, const void*, TickType_t) { return 0; }
size_t uxQueueMessagesWaiting(QueueHandle_t) { return 0; }
void vQueueDelete(QueueHandle_t) {}
SemaphoreHandle_t xSemaphoreCreateBinary() { return nullptr; }
int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return 0; }
int xSemaphoreGive(SemaphoreHandle_t) { return 0; }
void vSemaphoreDelete(SemaphoreHandle_t) {}
int xTaskCreate(void (*fn)(void*), const char* name, size_t, void* arg, unsigned, void*) {
    if (task_failure) return 0;
    assert(std::strcmp(name, "ap_stop") == 0 || std::strcmp(name, "ap_expiry") == 0);
    std::lock_guard<std::mutex> lock(workers_mutex);
    workers.emplace_back([fn, arg] { fn(arg); }); ++tasks; return pdPASS;
}
int xTaskCreatePinnedToCore(void (*fn)(void*), const char* n, size_t s, void* a, unsigned p, void* h, int) {
    return xTaskCreate(fn, n, s, a, p, h);
}
void vTaskDelete(void*) {}
WifiManager& WifiManager::GetInstance() { static WifiManager w; return w; }
bool WifiManager::IsInitialized() const { return manager_initialized.load(); }
void WifiManager::StopStation() {}
SdCardStorage& SdCardStorage::GetInstance() { static SdCardStorage s; return s; }
bool SdCardStorage::GetRecordingFilePath(const std::string&, std::string&, uint32_t&) { return false; }
bool SdCardStorage::ReadRecordingMetadata(const std::string&, uint32_t, size_t, std::vector<uint8_t>& out, uint32_t& size) {
    metadata_read(); out = {'{', '}'}; size = 2; return true;
}
AudreamPowerManager& AudreamPowerManager::GetInstance() { static AudreamPowerManager p; return p; }
void AudreamPowerManager::SetTemporaryAPTransferActive(bool active) { powered = active; }
BatteryTelemetryService& BatteryTelemetryService::GetInstance() { static BatteryTelemetryService s; return s; }
void BatteryTelemetryService::RecordEvent(BatteryTelemetryEvent) {
    assert(!TemporaryAPTransferService::GetInstance().IsActive());
    ++stopped_events;
}

int main() {
    using namespace std::chrono_literals;
    auto& service = TemporaryAPTransferService::GetInstance();
    auto& gate = GetWatchWifiGate();
    auto session = service.Start();
    assert(session.valid && service.IsActive() && powered && gate.TransferActive());
    assert(service.StopWithin(1000)); JoinTasks();
    assert(!service.IsActive() && !powered && !gate.TransferActive() && deinit_calls == 0);
    std::puts("PASS: actual AP service retains manager-owned driver and releases successful session");

    session = service.Start();
    std::promise<void> reading, release_read, read_done;
    auto release = release_read.get_future();
    auto done = read_done.get_future();
    metadata_read = [&] { reading.set_value(); release.wait(); };
    httpd_req_t request{metadata_handler.user_ctx, session.token};
    std::thread reader([&] { metadata_handler.handler(&request); read_done.set_value(); });
    reading.get_future().wait();
    socket_shutdown = [&] { release_read.set_value(); };
    http_stop = [&] {
        assert(done.wait_for(1s) == std::future_status::ready);
        return ESP_OK;
    };
    assert(service.StopWithin(1500)); reader.join(); JoinTasks();
    assert(!service.IsActive());
    metadata_read = [] {};
    socket_shutdown = [] {};
    std::puts("PASS: actual metadata handler releases its guard while HTTP shutdown waits outside the service lock");

    session = service.Start();
    std::promise<void> stopping, finish_stop;
    auto finish = finish_stop.get_future();
    http_stop = [&] { stopping.set_value(); finish.wait(); return ESP_OK; };
    const auto events_before = stopped_events.load();
    assert(service.Complete(session.token));
    stopping.get_future().wait();
    const auto before = std::chrono::steady_clock::now();
    assert(!service.StopWithin(20));
    assert(stopped_events == events_before);
    assert(std::chrono::steady_clock::now() - before < 500ms);
    assert(service.IsActive() && gate.TransferActive() && powered && !service.Start().valid);
    assert(!service.Complete("invalid-token"));
    httpd_req_t late{metadata_handler.user_ctx, session.token};
    assert(metadata_handler.handler(&late) == ESP_FAIL && late.error == 403);
    finish_stop.set_value();
    assert(service.StopWithin(1000)); JoinTasks();
    assert(stopped_events == events_before + 1);
    std::puts("PASS: bounded stop timeout retains ownership and denies new downloads and session replacement");

    http_stop = [] { return ESP_FAIL; };
    session = service.Start(); assert(session.valid);
    assert(!service.StopWithin(1000)); JoinTasks();
    assert(service.IsActive() && powered && gate.TransferActive());
    http_stop = [] { return ESP_OK; };
    assert(service.StopWithin(1000)); JoinTasks();
    std::puts("PASS: HTTP stop failure retains the handle and permits checked retry");

    session = service.Start(); assert(session.valid);
    task_failure = true;
    assert(!service.Complete(session.token) && !service.StopWithin(20));
    assert(service.IsActive() && powered);
    task_failure = false;
    assert(service.StopWithin(1000)); JoinTasks();
    std::puts("PASS: cleanup task allocation failure does not reopen a stopping batch");

    session = service.Start(); assert(session.valid);
    stop_error = ESP_FAIL;
    assert(!service.StopWithin(1000)); JoinTasks();
    const int calls = stop_calls;
    assert(service.IsActive() && gate.TransferActive());
    stop_error = ESP_OK;
    assert(service.StopWithin(1000)); JoinTasks();
    assert(stop_calls == calls);
    std::puts("PASS: radio failure retains ownership without stopping a freed HTTP handle twice");

    session = service.Start(); assert(session.valid);
    timer.callback(timer.arg); JoinTasks();
    assert(service.IsActive());
    now = 50000000;
    timer.callback(timer.arg); JoinTasks();
    assert(!service.IsActive());
    std::puts("PASS: stale expiry callback cannot stop an unexpired batch");

    manager_initialized = false;
    session = service.Start(); assert(session.valid);
    assert(service.StopWithin(1000)); JoinTasks();
    assert(deinit_calls == 1 && !powered);
    std::puts("PASS: standalone recording profile releases its own WiFi driver");

    std::promise<void> starting, release_start;
    auto startup_release = release_start.get_future();
    wifi_start = [&] { starting.set_value(); startup_release.wait(); };
    std::thread starter([&] { assert(!service.Start().valid); });
    starting.get_future().wait();
    const auto lock_before = std::chrono::steady_clock::now();
    assert(!service.StopWithin(20));
    assert(std::chrono::steady_clock::now() - lock_before < 500ms);
    release_start.set_value();
    starter.join();
    assert(service.StopWithin(1000)); JoinTasks();
    wifi_start = [] {};
    std::puts("PASS: stop deadline includes lock contention and fences in-flight AP startup");

    task_failure = true;
    http_start_error = ESP_FAIL;
    assert(!service.Start().valid);
    assert(service.IsActive() && gate.TransferActive() && powered);
    assert(!service.Start().valid);
    task_failure = false;
    assert(service.StopWithin(1000)); JoinTasks();
    assert(!service.IsActive() && !gate.TransferActive() && !powered);
    http_start_error = 0;
    assert(service.Start().valid);
    assert(service.StopWithin(1000)); JoinTasks();
    std::puts("PASS: failed HTTP startup and stop scheduling retain the AP until cleanup retry");

    assert(service.Start().valid);
    std::promise<void> renewing, release_renewal;
    auto renewal_release = release_renewal.get_future();
    timer_start = [&] { renewing.set_value(); renewal_release.wait(); };
    std::thread renewer([&] { assert(!service.Start().valid); });
    renewing.get_future().wait();
    assert(!service.StopWithin(20));
    release_renewal.set_value();
    renewer.join();
    assert(service.StopWithin(1000)); JoinTasks();
    timer_start = [] {};
    std::puts("PASS: stop intent also invalidates an in-flight batch renewal");

    AudioSessionCoordinator coordinator;
    std::atomic<bool> prepare_ok{true};
    std::atomic<int> prepare_calls{0}, release_calls{0}, finished_calls{0};
    std::function<void()> preparing = [] {};
    assert(!service.SetAudioOwnership(coordinator, {}, [] {}));
    assert(!service.Start().valid);
    assert(service.SetAudioOwnership(coordinator, [&](uint64_t generation, uint32_t timeout) {
        assert(timeout == 3000);
        ++prepare_calls;
        preparing();
        return prepare_ok && coordinator.HoldsExclusiveResources(generation);
    }, [&] { ++release_calls; }, [&] {
        assert(!service.IsActive());
        ++finished_calls;
    }));
    assert(!service.SetAudioOwnership(coordinator, [](uint64_t, uint32_t) { return true; }, [] {}));
    std::puts("PASS: watch ownership is mandatory after opt in and cannot be replaced at runtime");

    auto assistant = coordinator.Begin(AudioSessionMode::kAssistantConnecting);
    assert(coordinator.Complete(assistant));
    assert(coordinator.UpdateAssistant(AudioSessionMode::kAssistantListening, assistant.generation));
    session = service.Start();
    assert(session.valid && prepare_calls == 1 && coordinator.Mode() == AudioSessionMode::kFileTransfer);
    assert(!coordinator.Begin(AudioSessionMode::kAssistantConnecting));
    assert(!coordinator.EndAssistant(assistant.generation));
    assert(service.StopWithin(1000)); JoinTasks();
    assert(coordinator.Mode() == AudioSessionMode::kIdle && release_calls == 1);
    assert(finished_calls == 1);
    assert(service.StopWithin(1000)); JoinTasks();
    assert(finished_calls == 1);
    std::puts("PASS: transfer preempts the assistant and retains exclusive audio ownership until cleanup");

    for (auto mode : {AudioSessionMode::kLocalRecording, AudioSessionMode::kMaintenance, AudioSessionMode::kSleeping}) {
        const auto owned = coordinator.Begin(mode);
        assert(coordinator.Complete(owned));
        assert(!service.Start().valid);
        if (mode == AudioSessionMode::kLocalRecording) {
            const auto paused = coordinator.Begin(AudioSessionMode::kRecordingPaused);
            assert(coordinator.Complete(paused));
            assert(!service.Start().valid);
        }
        if (mode == AudioSessionMode::kSleeping) assert(coordinator.RecoverToIdle(true));
        else assert(coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)));
    }
    std::puts("PASS: recording paused recording maintenance and sleep reject transfer before WiFi starts");

    prepare_ok = false;
    assert(!service.Start().valid);
    assert(!service.StopWithin(1000)); JoinTasks();
    assert(service.IsActive() && !gate.TransferActive() && coordinator.Mode() == AudioSessionMode::kFault);
    assert(finished_calls == 1);
    prepare_ok = true;
    assert(service.StopWithin(1000)); JoinTasks();
    assert(coordinator.Mode() == AudioSessionMode::kIdle && release_calls == 2);
    assert(finished_calls == 2);
    std::puts("PASS: failed assistant handoff retains a recoverable lease even before AP startup");

    assert(service.Start().valid);
    const auto transfer_generation = coordinator.Generation();
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    assert(recording && !coordinator.ReleaseFileTransfer(transfer_generation));
    assert(service.StopWithin(1000)); JoinTasks();
    assert(coordinator.IsCurrent(recording.generation) && coordinator.Mode() == AudioSessionMode::kTransitioning);
    assert(release_calls == 2);
    assert(finished_calls == 3);
    assert(coordinator.Complete(recording));
    assert(coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)));
    std::puts("PASS: recording preemption cleanup cannot clear the newer recording lease or resume the assistant");

    std::promise<void> handoff_entered, release_handoff;
    auto handoff_release = release_handoff.get_future();
    preparing = [&] { handoff_entered.set_value(); handoff_release.wait(); };
    std::thread pending_transfer([&] { assert(!service.Start().valid); });
    handoff_entered.get_future().wait();
    const auto urgent_recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    assert(urgent_recording);
    assert(!service.StopWithin(20));
    release_handoff.set_value();
    pending_transfer.join();
    assert(service.StopWithin(1000)); JoinTasks();
    assert(coordinator.IsCurrent(urgent_recording.generation) && !gate.TransferActive());
    assert(coordinator.Complete(urgent_recording));
    assert(coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)));
    preparing = [] {};
    std::puts("PASS: recording can preempt transfer preparation without allowing stale AP startup");

    prepare_ok = false;
    assert(!service.Start().valid);
    assert(!service.StopWithin(1000)); JoinTasks();
    const auto fault_recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    assert(fault_recording);
    assert(service.StopWithin(1000)); JoinTasks();
    assert(coordinator.IsCurrent(fault_recording.generation));
    assert(coordinator.Fail(fault_recording, false));
    assert(!coordinator.Begin(AudioSessionMode::kLocalRecording));
    assert(!service.Start().valid);
    assert(coordinator.Complete(coordinator.BeginFaultCleanup(fault_recording.generation)));
    prepare_ok = true;
    assert(release_calls == 2);
    std::puts("PASS: recording inherits transfer cleanup responsibility but cannot bypass its own failed audio cleanup");
}
