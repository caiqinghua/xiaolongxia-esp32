#include "boards/common/blufi_session_state.h"
#include "boards/common/blufi_control_session.h"
#include "boards/common/blufi_profile_startup.h"
#include "esp_bt_main.h"
#include "esp_bt.h"
#include <cassert>
#include <cstdio>
#include <functional>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// Platform I/O fixtures. This tests lifecycle ordering, not SDK ABI or RF state.
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
namespace {
std::vector<std::string> calls;
std::string failure;
std::string cleanup_failure;
std::string stuck_operation;
int64_t now_ms = 0;
esp_bluedroid_status_t host_status = ESP_BLUEDROID_STATUS_UNINITIALIZED;
esp_bt_controller_status_t controller_status = ESP_BT_CONTROLLER_STATUS_IDLE;
std::function<void()> on_host_disable;
int Call(const std::string& name) {
    calls.push_back(name);
    return name == failure || name == cleanup_failure ? ESP_FAIL : ESP_OK;
}
void Reset() {
    calls.clear(); failure.clear(); cleanup_failure.clear(); stuck_operation.clear(); now_ms = 0;
    host_status = ESP_BLUEDROID_STATUS_UNINITIALIZED;
    controller_status = ESP_BT_CONTROLLER_STATUS_IDLE;
    on_host_disable = {};
}
bool Called(const char* name) { return std::find(calls.begin(), calls.end(), name) != calls.end(); }
}
int64_t esp_timer_get_time() { return now_ms * 1000; }
int esp_bluedroid_init() {
    auto rc = Call("host.init"); if (rc == ESP_OK) host_status = ESP_BLUEDROID_STATUS_INITIALIZED; return rc;
}
int esp_bluedroid_enable() {
    auto rc = Call("host.enable"); if (rc == ESP_OK) host_status = ESP_BLUEDROID_STATUS_ENABLED; return rc;
}
esp_bluedroid_status_t esp_bluedroid_get_status() { return host_status; }
int esp_bluedroid_disable() {
    if (on_host_disable) on_host_disable();
    auto rc = Call("host.disable");
    if (rc == ESP_OK && stuck_operation != "host.disable") host_status = ESP_BLUEDROID_STATUS_INITIALIZED;
    return rc;
}
int esp_bluedroid_deinit() {
    auto rc = Call("host.deinit");
    if (rc == ESP_OK && stuck_operation != "host.deinit") host_status = ESP_BLUEDROID_STATUS_UNINITIALIZED;
    return rc;
}
esp_bt_controller_status_t esp_bt_controller_get_status() { return controller_status; }
int esp_bt_controller_disable() {
    auto rc = Call("controller.disable");
    if (rc == ESP_OK && stuck_operation != "controller.disable") controller_status = ESP_BT_CONTROLLER_STATUS_INITED;
    return rc;
}
int esp_bt_controller_deinit() {
    auto rc = Call("controller.deinit");
    if (rc == ESP_OK && stuck_operation != "controller.deinit") controller_status = ESP_BT_CONTROLLER_STATUS_IDLE;
    return rc;
}
int esp_blufi_profile_deinit() { return Call("profile.stop"); }
struct esp_blufi_callbacks_t {
    void (*event_cb)(); void (*negotiate_data_handler)(); void (*encrypt_func)();
    void (*decrypt_func)(); void (*checksum_func)();
};
int esp_blufi_register_callbacks(esp_blufi_callbacks_t*) { return Call("callbacks"); }
void StartAudreamBluFiAdvertising() { Call("advertise"); }
class Owner {
public:
    explicit Owner(const char* name) : name_(name) {}
    bool HostStarted() { return Call(name_ + ".start") == ESP_OK; }
    void HostStopping() { Call(name_ + ".stopping"); }
    void HostStopped() { Call(name_ + ".stopped"); }
private:
    std::string name_;
};
struct WatchAncsClient { static Owner& Get() { static Owner o("native"); return o; } };
struct WatchAndroidGattServer { static Owner& Get() { static Owner o("android"); return o; } };
struct WatchPairingGattServer { static Owner& Get() { static Owner o("pairing"); return o; } };
struct WifiGate {
    bool Enabled() const { return false; }
    bool TransferActive() const { return false; }
    bool Run(const std::function<bool()>& op) { return op(); }
};
WifiGate& GetWatchWifiGate() { static WifiGate gate; return gate; }
struct WifiManager {
    static WifiManager& GetInstance() { static WifiManager manager; return manager; }
    bool IsInitialized() const { return false; }
    bool IsConfigMode() const { return false; }
    void StopStation() { assert(false); }
};
class Blufi {
public:
    std::mutex lifecycle_mutex_, credentials_mutex_;
    uint64_t control_lease_ = 0, next_control_lease_ = 0;
    bool shutdown_failed_ = false;
    BlufiSessionState session_;
    BlufiControlSession control_session_{true};
    BlufiProfileStartup profile_startup_;
    bool m_ble_is_connected = false;
    int m_sta_config = 0;
    bool m_ssid_valid = false, m_password_valid = false;
    int scan_starts = 0;
    void start_wifi_scan();
    void StartWifiScanAllowed() { ++scan_starts; }
    esp_err_t init(BlufiRole role);
    esp_err_t deinit();
    esp_err_t DeinitLocked();
    void HandleProfileInitialized(bool success);
    bool IsControlReady() const;
    uint64_t AcquireControlLease();
    bool ReleaseControlLease(uint64_t lease);
    static esp_err_t _host_init();
    static esp_err_t _host_and_cb_init();
    static esp_err_t AbortSecureHostStart();
    static esp_err_t _controller_init() {
        const auto rc = Call("controller.init");
        controller_status = rc == ESP_OK ? ESP_BT_CONTROLLER_STATUS_ENABLED : ESP_BT_CONTROLLER_STATUS_INITED;
        return rc;
    }
    static esp_err_t _controller_deinit();
    static esp_err_t _host_deinit();
    static esp_err_t _gap_register_callback() { return Call("profile.enqueue"); }
    static void _event_callback_trampoline() {}
    static void _negotiate_data_handler_trampoline() {}
    static void _encrypt_func_trampoline() {}
    static void _decrypt_func_trampoline() {}
    static void _checksum_func_trampoline() {}
};
#include "methods.inc"

static void TestProfileDeadline() {
    BlufiProfileStartup profile;
    assert(!profile.Ready(0) && !profile.Failed(0));
    assert(!profile.Complete(true, 0));
    profile.Begin(0);
    assert(!profile.Ready(59999) && !profile.Failed(59999));
    assert(profile.Complete(true, 59999));
    assert(profile.Ready(120000) && !profile.Failed(120000));
    assert(!profile.Complete(false, 120000) && profile.Ready(120000));
    for (const auto start : {int64_t{0}, int64_t{1000}, std::numeric_limits<int64_t>::max() - 60000}) {
        profile.Begin(start);
        assert(!profile.Ready(start + 59999) && !profile.Failed(start + 59999));
        assert(!profile.Complete(true, start + 60000) && profile.Failed(start + 60000));
        assert(!profile.Complete(true, start + 59999));
        profile.Begin(start);
        assert(!profile.Ready(start + 60000) && profile.Failed(start + 60000));
        assert(!profile.Complete(true, start + 60000));
    }
    profile.Begin(1000);
    assert(!profile.Complete(true, 999) && profile.Failed(999));
    profile.Begin(-1);
    assert(profile.Failed(0) && !profile.Complete(true, 0));
    profile.Begin(1000);
    profile.Fail();
    assert(!profile.Complete(true, 1001));
    profile.Stop();
    assert(!profile.Complete(true, 1001) && !profile.Failed(1001));
    for (int i = 0; i < 256; ++i) {
        profile.Begin(1000);
        std::thread callback([&] { profile.Complete(true, 1001); });
        std::thread stop([&] { profile.Stop(); });
        callback.join();
        stop.join();
        assert(!profile.Ready(1001) && !profile.Failed(1001));
    }
    std::puts("PASS: profile deadline boundary, rollback, stopped callbacks and concurrent completion");
}

#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
static void TestShutdown() {
    Reset();
    assert(Blufi::_controller_deinit() == ESP_OK && calls.empty());
    controller_status = ESP_BT_CONTROLLER_STATUS_INITED;
    assert(Blufi::_controller_deinit() == ESP_OK);
    assert(!Called("controller.disable") && Called("controller.deinit"));
    assert(controller_status == ESP_BT_CONTROLLER_STATUS_IDLE);
    Reset(); controller_status = ESP_BT_CONTROLLER_STATUS_NUM;
    assert(Blufi::_controller_deinit() == ESP_ERR_INVALID_STATE && calls.empty());
    for (const auto state : {ESP_BLUEDROID_STATUS_INITIALIZED, ESP_BLUEDROID_STATUS_ENABLED,
                             ESP_BLUEDROID_STATUS_DISABLING}) {
        Reset(); host_status = state; controller_status = ESP_BT_CONTROLLER_STATUS_ENABLED;
        assert(Blufi::_controller_deinit() == ESP_ERR_INVALID_STATE && calls.empty());
    }
    for (const auto* point : {"profile.stop", "host.disable", "host.deinit", "controller.disable", "controller.deinit"}) {
        Reset();
        Blufi app;
        assert(app.init(BlufiRole::kControlOnly) == ESP_OK);
        failure = point;
        assert(app.DeinitLocked() != ESP_OK && app.shutdown_failed_);
        assert(!app.session_.IsActive() && !app.IsControlReady() && !app.AcquireControlLease());
        const bool host_stopped = failure.find("controller.") == 0;
        assert(Called("native.stopped") == host_stopped);
        assert(Called("android.stopped") == host_stopped);
        assert(Called("pairing.stopped") == host_stopped);
        if (!host_stopped) assert(!Called("controller.disable") && !Called("controller.deinit"));
        if (failure == "controller.disable") assert(!Called("controller.deinit"));
        const auto count = calls.size();
        failure.clear();
        app.HandleProfileInitialized(true);
        assert(app.init(BlufiRole::kControlOnly) == ESP_ERR_INVALID_STATE && calls.size() == count);
        assert(!Called("advertise"));
    }
    for (const auto* point : {"host.disable", "host.deinit", "controller.disable", "controller.deinit"}) {
        Reset();
        Blufi app;
        assert(app.init(BlufiRole::kControlOnly) == ESP_OK);
        stuck_operation = point;
        assert(app.DeinitLocked() != ESP_OK && app.shutdown_failed_);
        if (stuck_operation == "host.disable") assert(!Called("host.deinit"));
        if (stuck_operation.find("host.") == 0) {
            assert(!Called("native.stopped") && !Called("controller.disable") && !Called("controller.deinit"));
        }
        if (stuck_operation == "controller.disable") assert(!Called("controller.deinit"));
        const auto count = calls.size();
        stuck_operation.clear();
        assert(app.init(BlufiRole::kControlOnly) == ESP_ERR_INVALID_STATE && calls.size() == count);
    }
    Reset();
    Blufi app;
    assert(app.init(BlufiRole::kControlOnly) == ESP_OK);
    now_ms = BlufiProfileStartup::kTimeoutMs;
    assert(!app.IsControlReady() && app.profile_startup_.Failed(now_ms));
    assert(app.DeinitLocked() == ESP_OK && !app.shutdown_failed_);
    assert(host_status == ESP_BLUEDROID_STATUS_UNINITIALIZED && controller_status == ESP_BT_CONTROLLER_STATUS_IDLE);
    for (const auto* name : {"native", "android", "pairing"}) {
        const auto stopping = std::find(calls.begin(), calls.end(), std::string(name) + ".stopping");
        const auto stopped = std::find(calls.begin(), calls.end(), std::string(name) + ".stopped");
        assert(stopping < std::find(calls.begin(), calls.end(), "profile.stop"));
        assert(std::find(calls.begin(), calls.end(), "host.deinit") < stopped);
        assert(stopped < std::find(calls.begin(), calls.end(), "controller.disable"));
    }
    const auto count = calls.size();
    assert(app.DeinitLocked() == ESP_OK && calls.size() == count);
    assert(app.init(BlufiRole::kControlOnly) == ESP_OK);
    app.HandleProfileInitialized(true);
    assert(app.IsControlReady());
    assert(app.DeinitLocked() == ESP_OK);
    std::puts("PASS: actual host/controller teardown ordering, failures, state checks and restart inhibition");
}

static void TestScanDuringShutdown() {
    Reset();
    Blufi app;
    assert(app.init(BlufiRole::kControlOnly) == ESP_OK);
    app.HandleProfileInitialized(true);
    app.start_wifi_scan();
    assert(app.scan_starts == 1);
    on_host_disable = [&] {
        // Emulate a queued BTC request ahead of the host-disable completion.
        std::promise<void> returned;
        auto ready = returned.get_future();
        std::thread btc([&] { app.start_wifi_scan(); returned.set_value(); });
        assert(ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        btc.join();
        assert(app.scan_starts == 1);
    };
    assert(app.deinit() == ESP_OK);
    on_host_disable = {};
    assert(!app.shutdown_failed_ && !app.IsControlReady());
    assert(host_status == ESP_BLUEDROID_STATUS_UNINITIALIZED && controller_status == ESP_BT_CONTROLLER_STATUS_IDLE);
    assert(app.init(BlufiRole::kControlOnly) == ESP_OK);
    app.HandleProfileInitialized(true);
    app.start_wifi_scan();
    assert(app.scan_starts == 2);
    assert(app.deinit() == ESP_OK);
    std::puts("PASS: queued BTC scan cannot deadlock host shutdown or begin work under its lifecycle lock");
}
#endif

int main() {
    TestProfileDeadline();
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    TestShutdown();
    TestScanDuringShutdown();
#endif
    Reset();
    Blufi good;
    assert(good.init(BlufiRole::kControlOnly) == ESP_OK);
    assert(good.session_.IsActive() && !good.session_.AllowsProvisioning());
    assert(Called("native.start") && Called("android.start") && Called("profile.enqueue"));
    const auto count = calls.size();
    assert(good.init(BlufiRole::kControlOnly) == ESP_OK && calls.size() == count);
    assert(good.init(BlufiRole::kWifiProvisioning) == ESP_ERR_INVALID_STATE);
    assert(!Called("host.disable"));
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    assert(Called("pairing.start"));
    assert(!good.IsControlReady() && !Called("advertise"));
    assert(!good.AcquireControlLease());
    good.HandleProfileInitialized(true);
    assert(good.IsControlReady() && Called("advertise"));
    auto ready_count = calls.size();
    good.HandleProfileInitialized(true);
    good.HandleProfileInitialized(false);
    assert(good.IsControlReady() && calls.size() == ready_count);
    const auto control_lease = good.AcquireControlLease();
    assert(control_lease && !good.AcquireControlLease());
    assert(good.DeinitLocked() == ESP_ERR_INVALID_STATE && good.IsControlReady());
    assert(good.ReleaseControlLease(control_lease));
    assert(good.DeinitLocked() == ESP_OK && !good.IsControlReady());
    ready_count = calls.size();
    good.HandleProfileInitialized(true);
    assert(!good.IsControlReady() && calls.size() == ready_count);

    Reset();
    Blufi async_failed;
    assert(async_failed.init(BlufiRole::kControlOnly) == ESP_OK);
    async_failed.HandleProfileInitialized(false);
    async_failed.HandleProfileInitialized(true);
    assert(!async_failed.IsControlReady() && !Called("advertise"));
    assert(!async_failed.AcquireControlLease());
    auto failed_count = calls.size();
    assert(async_failed.init(BlufiRole::kControlOnly) == ESP_FAIL && calls.size() == failed_count);
    assert(async_failed.DeinitLocked() == ESP_OK);
    assert(async_failed.init(BlufiRole::kControlOnly) == ESP_OK);
    assert(!async_failed.IsControlReady());
    async_failed.HandleProfileInitialized(true);
    assert(async_failed.IsControlReady());
    for (int entry = 0; entry < 3; ++entry) {
        Reset(); now_ms = 1000;
        Blufi timed_out;
        assert(timed_out.init(BlufiRole::kControlOnly) == ESP_OK);
        now_ms += 59999;
        assert(!timed_out.IsControlReady() && !timed_out.AcquireControlLease());
        assert(timed_out.init(BlufiRole::kControlOnly) == ESP_OK);
        const auto before = calls.size();
        ++now_ms;
        if (entry == 0) assert(!timed_out.IsControlReady());
        if (entry == 1) assert(timed_out.init(BlufiRole::kControlOnly) == ESP_FAIL);
        if (entry == 2) timed_out.HandleProfileInitialized(true);
        assert(timed_out.profile_startup_.Failed(now_ms));
        timed_out.HandleProfileInitialized(true);
        assert(!timed_out.IsControlReady() && !timed_out.AcquireControlLease());
        assert(timed_out.init(BlufiRole::kControlOnly) == ESP_FAIL);
        assert(calls.size() == before && !Called("advertise"));
        assert(timed_out.DeinitLocked() == ESP_OK);
        assert(timed_out.init(BlufiRole::kControlOnly) == ESP_OK);
        timed_out.HandleProfileInitialized(true);
        assert(timed_out.IsControlReady());
        assert(timed_out.DeinitLocked() == ESP_OK);
    }
    for (const auto* point : {"controller.init", "host.init", "host.enable", "callbacks",
                             "native.start", "android.start", "pairing.start", "profile.enqueue"}) {
        Reset(); failure = point;
        Blufi app;
        assert(app.init(BlufiRole::kControlOnly) == ESP_FAIL);
        assert(!app.session_.IsActive() && !app.shutdown_failed_);
        assert(host_status == ESP_BLUEDROID_STATUS_UNINITIALIZED);
        assert(Called("controller.deinit"));
        if (failure != "profile.enqueue") assert(!Called("profile.enqueue"));
        if (failure == "native.start") assert(!Called("android.start") && !Called("pairing.start"));
        if (failure == "android.start") assert(!Called("pairing.start"));
        if (failure != "controller.init") {
            assert(Called("native.stopping") && Called("native.stopped"));
            assert(Called("android.stopped") && Called("pairing.stopped"));
            const auto stopped = std::find(calls.begin(), calls.end(), "native.stopped");
            assert(stopped < std::find(calls.begin(), calls.end(), "controller.deinit"));
        }
        failure.clear();
        assert(app.init(BlufiRole::kControlOnly) == ESP_OK);
    }
    for (const auto* point : {"host.disable", "host.deinit", "controller.deinit"}) {
        Reset(); failure = "native.start"; cleanup_failure = point;
        Blufi app;
        assert(app.init(BlufiRole::kControlOnly) == ESP_FAIL);
        assert(!app.session_.IsActive() && app.shutdown_failed_);
        if (cleanup_failure != "controller.deinit") {
            assert(!Called("controller.deinit") && !Called("native.stopped"));
        }
        const auto failed_count = calls.size();
        failure.clear(); cleanup_failure.clear();
        assert(app.init(BlufiRole::kControlOnly) == ESP_ERR_INVALID_STATE);
        assert(calls.size() == failed_count);
    }
    Reset(); failure = "controller.init"; cleanup_failure = "controller.deinit";
    Blufi failed_controller;
    assert(failed_controller.init(BlufiRole::kControlOnly) == ESP_FAIL && failed_controller.shutdown_failed_);
    // An in-progress/unknown SDK state must not be treated as a stopped host.
    Reset(); host_status = ESP_BLUEDROID_STATUS_DISABLING;
    assert(Blufi::AbortSecureHostStart() == ESP_FAIL);
    assert(!Called("native.stopped") && !Called("host.deinit"));
#else
    assert(good.IsControlReady());
    // Existing notification-only builds retain their startup policy.
    Reset(); failure = "native.start";
    Blufi legacy;
    assert(legacy.init(BlufiRole::kControlOnly) == ESP_OK);
    assert(Called("android.start") && Called("profile.enqueue") && !Called("pairing.start"));
#endif
    std::printf("BLUFI startup and rollback passed: secure=%d\n", CONFIG_AUDREAM_WATCH_DEVICE_PAIRING);
}
