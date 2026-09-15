#include "application.h"
#include "board.h"
#include "watch/watch_wifi_gate.h"
#include "system/audream_power_manager.h"
#include "system/device_time_service.h"
#include "wifi_station.h"
#include "esp_timer.h"
#include <cassert>
#include <cstdio>
#include <sys/time.h>
#include <thread>

using namespace watch_voice;
using namespace std::chrono_literals;
namespace {
const Binding initial{1, "12345678-1234-1234-1234-123456789abc", "adv_" + std::string(43, 'x')};
Binding persisted = initial;
BindingStatus persisted_status = BindingStatus::kBound;
std::mutex request_mutex;
std::condition_variable changed;
bool entered = false, release_request = false, enable_response = true;
std::thread::id main_thread;
int clock_updates = 0;
bool clock_failure = false;
std::atomic<unsigned> acknowledgements{0};
std::string acknowledged_error;
Result acknowledgement_result = Result::kOk;
std::string activation_mode = "push_to_talk";
std::string power_mode = "watch_standby";
std::atomic<bool> acknowledgement_entered{false};
bool block_acknowledgement = false, release_acknowledgement = false;
SettingsReceipt persisted_receipt;
ReceiptStatus receipt_load_result = ReceiptStatus::kAbsent;
ReceiptStatus receipt_save_result = ReceiptStatus::kOk;
unsigned receipt_saves = 0;
bool rebind_during_restore = false;
uint64_t server_revision = 1;
unsigned power_updates = 0;
Result power_update_result = Result::kOk;
bool commit_power_on_failure = false;
bool block_power_update = false, release_power_update = false;
std::atomic<bool> power_update_entered{false};
std::atomic<bool> power_update_guard_checked{false};

void Reset() {
    assert(AudreamPowerManager::GetInstance().network_leases == 0);
    assert(AudreamPowerManager::GetInstance().storage_leases == 0);
    AudreamPowerManager::GetInstance().state = AudreamPowerState::kActive;
    AudreamPowerManager::GetInstance().saver = false;
    AudreamPowerManager::GetInstance().policy_allowed = true;
    GetWatchWifiGate().Initialize(true);
    Board::GetInstance() = Board{};
    WifiStation::GetInstance().connected = true;
    DeviceTimeService::GetInstance().synchronized = true;
    persisted = initial;
    persisted_status = BindingStatus::kBound;
    entered = release_request = false;
    enable_response = true;
    clock_updates = 0;
    clock_failure = false;
    acknowledgements = 0;
    acknowledged_error.clear();
    acknowledgement_result = Result::kOk;
    activation_mode = "push_to_talk";
    power_mode = "watch_standby";
    acknowledgement_entered = false;
    block_acknowledgement = release_acknowledgement = false;
    persisted_receipt = {};
    receipt_load_result = ReceiptStatus::kAbsent;
    receipt_save_result = ReceiptStatus::kOk;
    receipt_saves = 0;
    rebind_during_restore = false;
    server_revision = 1;
    power_updates = 0;
    power_update_result = Result::kOk;
    commit_power_on_failure = false;
    block_power_update = release_power_update = false;
    power_update_entered = false;
    power_update_guard_checked = false;
    Assets::GetInstance().available = true;
    Assets::GetInstance().models_ready = false;
    Assets::GetInstance().health_begins = 0;
    Assets::GetInstance().health_polls = 0;
    Assets::GetInstance().last_healthy = false;
    Assets::GetInstance().keep_observing = true;
    test_voice_now_us = 10000000;
}

void AwaitEntry() {
    std::unique_lock<std::mutex> lock(request_mutex);
    assert(changed.wait_for(lock, 2s, []() { return entered; }));
}
void Release() {
    std::lock_guard<std::mutex> lock(request_mutex);
    release_request = true;
    changed.notify_all();
}
void Finish(Application& app) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((app.watch_cloud_task_.Active() || app.watch_cloud_application_) &&
           std::chrono::steady_clock::now() < deadline) {
        app.PollWatchBootstrap();
        std::this_thread::sleep_for(1ms);
    }
    assert(!app.watch_cloud_task_.Active());
    assert(!app.watch_cloud_application_);
    assert(AudreamPowerManager::GetInstance().network_leases == 0);
}
}

// Never set the host clock. Exercise the production preflight call contract.
extern "C" int settimeofday(const timeval* value, const struct timezone*) {
    assert(value->tv_sec == 1789401600 && value->tv_usec == 0);
    ++clock_updates;
    return clock_failure ? -1 : 0;
}

namespace watch_voice {
BindingStatus LoadBinding(Binding& result) { result = persisted; return persisted_status; }
uint64_t ActiveBindingGeneration() { return persisted_status == BindingStatus::kBound ? persisted.generation : 0; }
bool WithBoundIdentity(const std::function<bool(std::string_view, uint64_t)>& verifier) {
    if (rebind_during_restore) {
        ++persisted.generation;
        receipt_load_result = ReceiptStatus::kAbsent;
        rebind_during_restore = false;
    }
    return persisted_status == BindingStatus::kBound && verifier(persisted.device_id, persisted.generation);
}
ReceiptStatus LoadSettingsReceipt(const Binding& binding, SettingsReceipt& result) {
    assert(binding.generation == persisted.generation && binding.device_id == persisted.device_id);
    assert(AudreamPowerManager::GetInstance().storage_leases == 1);
    result = receipt_load_result == ReceiptStatus::kOk ? persisted_receipt : SettingsReceipt{};
    return receipt_load_result;
}
ReceiptStatus SaveSettingsReceipt(const Binding& binding, uint64_t revision, bool enabled,
                                  std::string_view error, SettingsReceipt& result, std::string_view mode) {
    assert(binding.generation == persisted.generation && binding.device_id == persisted.device_id);
    assert(AudreamPowerManager::GetInstance().storage_leases == 1);
    ++receipt_saves;
    result = {};
    if (receipt_save_result != ReceiptStatus::kOk) return receipt_save_result;
    persisted_receipt = {revision, error.empty() ? revision : persisted_receipt.applied_revision,
                         enabled, std::string(error), std::string(mode),
                         error.empty() ? std::string(mode) : persisted_receipt.applied_power_mode};
    result = persisted_receipt;
    receipt_load_result = ReceiptStatus::kOk;
    return ReceiptStatus::kOk;
}
Result Client::FetchBootstrap(const std::string& id, const std::string& credential, int version,
                              bool valid_clock, const Cancelled& cancelled, Bootstrap& result) const {
    assert(std::this_thread::get_id() != main_thread);
    assert(id == initial.device_id && credential == initial.credential && version == 1 && valid_clock);
    assert(AudreamPowerManager::GetInstance().network_leases == 1);
    {
        std::unique_lock<std::mutex> lock(request_mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, []() { return release_request; });
    }
    if (cancelled()) return Result::kCancelled;
    result.protocol_version = 1;
    result.settings.enabled = enable_response;
    result.settings.activation_mode = activation_mode;
    result.settings.wake_word_enabled = activation_mode == "wake_word";
    result.settings.language = "en-US";
    result.settings.voice = "default";
    result.settings.power_mode = power_mode;
    result.settings.revision = server_revision;
    if (enable_response) {
        result.access_token = "eyJhbGci.test.signature";
        result.websocket_url = "wss://voice.audream.ai/v1/device";
        result.expires_at_ms = 3600000;
    }
    return Result::kOk;
}

Result Client::UpdatePowerMode(const std::string& id, const std::string& credential, uint64_t revision,
                              std::string_view mode, bool valid_clock, const Cancelled& cancelled,
                              Settings& result) const {
    assert(std::this_thread::get_id() != main_thread);
    assert(id == initial.device_id && credential == initial.credential && valid_clock);
    assert(AudreamPowerManager::GetInstance().network_leases == 1);
    ++power_updates;
    {
        std::unique_lock<std::mutex> lock(request_mutex);
        power_update_entered = true;
        changed.notify_all();
        if (block_power_update) changed.wait(lock, [] { return release_power_update; });
    }
    const bool invalidated = cancelled();
    power_update_guard_checked = true;
    if (invalidated) return Result::kCancelled;
    assert(revision == server_revision);
    if (power_update_result == Result::kOk || commit_power_on_failure) {
        ++server_revision;
        power_mode = mode;
        activation_mode = mode == "always_available" ? "wake_word" : "push_to_talk";
    }
    result.revision = server_revision;
    return power_update_result;
}

Result Client::Acknowledge(const std::string& id, const std::string& credential, uint64_t revision,
                           std::string_view error, bool valid_clock, const Cancelled& cancelled,
                           Settings& result) const {
    assert(std::this_thread::get_id() != main_thread);
    assert(id == initial.device_id && credential == initial.credential && revision == server_revision && valid_clock);
    assert(AudreamPowerManager::GetInstance().network_leases == 1);
    {
        std::unique_lock<std::mutex> lock(request_mutex);
        acknowledgement_entered = true;
        if (block_acknowledgement) changed.wait(lock, []() { return release_acknowledgement; });
    }
    if (cancelled()) return Result::kCancelled;
    if (error.empty()) assert(receipt_saves > 0 && persisted_receipt.applied_revision == revision);
    acknowledged_error = error;
    ++acknowledgements;
    result.revision = revision;
    result.applied_revision = error.empty() ? revision : 0;
    result.apply_error = error;
    return acknowledgement_result;
}
}

int main() {
    main_thread = std::this_thread::get_id();
    {
        Reset();
        Application app;
        auto& assets = Assets::GetInstance();
        auto& power = AudreamPowerManager::GetInstance();
        app.PollWatchAssetHealth(true);
        assert(assets.health_polls == 0);
        app.watch_asset_health_active_ = true;
        app.audio_service_.wake_running = true;
        app.PollWatchAssetHealth(true);
        assert(!assets.last_healthy);
        app.watch_boot_health_.Begin(true, 0);
        for (int i = 1; i <= 30; ++i)
            assert(app.watch_boot_health_.Observe(i * 1000, true) == (i == 30));
        app.watch_boot_health_.Complete(true);
        app.PollWatchAssetHealth(true);
        assert(assets.last_healthy && power.storage_leases == 0);
        app.PollWatchAssetHealth(false);
        assert(!assets.last_healthy);
        app.audio_service_.wake_running = false;
        app.PollWatchAssetHealth(true);
        assert(!assets.last_healthy);
        app.audio_service_.wake_running = true;
        power.allowed = false;
        app.PollWatchAssetHealth(true);
        assert(!assets.last_healthy && power.storage_leases == 0);
        power.allowed = true;
        assets.keep_observing = false;
        app.PollWatchAssetHealth(true);
        assert(assets.last_healthy && !app.watch_asset_health_active_ && power.storage_leases == 0);
        const auto polls = assets.health_polls;
        app.PollWatchAssetHealth(true);
        assert(assets.health_polls == polls);
    }
    std::puts("PASS: asset health owner requires firmware health, live wake input and a storage lease");
    {
        Reset();
        Application app;
        app.watch_firmware_busy_ = true;
        app.StartWatchBootstrap(initial);
        app.PollWatchBootstrap();
        assert(!app.watch_cloud_task_.Active() && !entered && !app.CanPrepareWatchNetwork());
        app.watch_wake_started_ = true;
        app.audio_service_.wake_running = true;
        app.PollWatchWakeListening();
        assert(app.watch_wake_retirement_ && !app.watch_wake_started_);
        app.PollWatchWakeListening();
        assert(!app.watch_wake_retirement_ && !app.audio_service_.wake_running);
        assert(app.audio_session_coordinator_.Mode() == AudioSessionMode::kIdle);
    }
    {
        Reset();
        Application app;
        auto& board = Board::GetInstance();
        app.protocol_ = std::make_unique<TestProtocol>();
        app.protocol_->close_ready = false;
        app.watch_cloud_ack_ = PendingAcknowledgement{};
        app.watch_station_policy_.CancelDemand();
        app.PollWatchNetwork();
        assert(app.watch_cloud_retirement_ && !GetWatchWifiGate().StationAllowed() && !app.watch_cloud_ack_);
        assert(board.station_stops == 0);
        app.PollWatchBootstrap();
        assert(app.protocol_ && app.watch_cloud_retirement_ && !app.watch_cloud_task_.Active());
        app.protocol_->close_ready = true;
        app.PollWatchBootstrap();
        assert(!app.protocol_ && !app.watch_cloud_retirement_ && !app.watch_cloud_task_.Active());
        board.stop_succeeds = false;
        app.PollWatchNetwork();
        assert(board.station_stops == 1 && !app.watch_station_policy_.Suspended());
        board.stop_succeeds = true;
        test_voice_now_us += 2000000;
        app.PollWatchNetwork();
        assert(board.station_stops == 2 && app.watch_station_policy_.Suspended());
        assert(GetWatchWifiGate().Enabled() && !WifiStation::GetInstance().IsConnected());
        assert(app.CanPrepareWatchNetwork() && app.RequestWatchNetwork());
        app.PollWatchNetwork();
        assert(board.station_starts == 1 && app.watch_network_preparing_);
        assert(!app.protocol_ && !app.has_assistant_session && !app.pending_assistant_listening_);
        app.PollWatchNetwork();
        assert(board.station_starts == 1 && !app.watch_cloud_task_.Active());
        WifiStation::GetInstance().connected = true;
        app.PollWatchBootstrap();
        AwaitEntry(); Release(); Finish(app);
        app.PollWatchNetwork();
        assert(app.protocol_ && !app.watch_network_preparing_ && app.watch_power_mode_.revision == 1);
        assert(!app.has_assistant_session && !app.pending_assistant_listening_ && app.audio_service_.wake_starts == 0);
        assert(AudreamPowerManager::GetInstance().storage_leases == 0);
    }
    std::puts("PASS: on-demand STA drains protocol, retries checked stop, reconnects and bootstraps without capture");
    for (int reason = 0; reason < 6; ++reason) {
        Reset();
        Application app;
        WifiStation::GetInstance().connected = false;
        assert(app.RequestWatchNetwork());
        std::optional<AudioSessionTransition> recording;
        if (reason == 0) ++persisted.generation;
        if (reason == 1) ++app.watch_voice_request_;
        if (reason == 2) GetWatchWifiGate().Initialize(false);
        if (reason == 3) recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
        if (reason == 4) assert(GetWatchWifiGate().BeginTransfer([] { return true; }));
        if (reason == 5) test_voice_now_us += 90000000;
        app.PollWatchNetwork();
        assert(!app.watch_network_preparing_ && !app.has_assistant_session);
        assert(Board::GetInstance().station_starts == 0);
        if (recording) {
            assert(Board::GetInstance().station_stops == 0);
            app.audio_session_coordinator_.Fail(*recording, true);
        }
        if (reason == 4) {
            assert(Board::GetInstance().station_stops == 0);
            assert(GetWatchWifiGate().EndTransfer([] { return true; }));
        }
    }
    std::puts("PASS: rebind, permission, recording, AP ownership and timeout cancel pending connection safely");
    {
        Reset();
        Application app;
        WifiStation::GetInstance().connected = false;
        auto& board = Board::GetInstance();
        board.credentials = false;
        assert(!app.RequestWatchNetwork());
        board.credentials = true;
        board.provisioning = true;
        assert(!app.RequestWatchNetwork());
        app.watch_station_policy_.CancelDemand();
        app.PollWatchNetwork();
        assert(board.station_starts == 0 && board.station_stops == 0);
        board.provisioning = false;
        app.audio_service_.enabled = false;
        assert(!app.RequestWatchNetwork());
        GetWatchWifiGate().SetStationAllowed(false);
        WifiStation::GetInstance().connected = true;
        app.StartWatchBootstrap(initial);
        assert(!app.watch_cloud_task_.Active());
    }
    std::puts("PASS: provisioning owns its connection and a stale connected bit cannot authorize cloud work");
    {
        Reset();
        Application app;
        auto& board = Board::GetInstance();
        WifiStation::GetInstance().connected = false;
        board.start_succeeds = false;
        assert(app.RequestWatchNetwork());
        for (int attempt = 0; attempt < 3; ++attempt) {
            app.PollWatchNetwork();
            test_voice_now_us += 2000000;
        }
        assert(board.station_starts == 3 && app.watch_network_failed_ && !app.watch_network_preparing_);
        app.PollWatchNetwork();
        assert(board.station_stops == 1 && app.watch_station_policy_.Suspended());
        board.start_succeeds = true;
        assert(app.RequestWatchNetwork() && !app.watch_network_failed_);
        app.PollWatchNetwork();
        assert(board.station_starts == 4 && app.watch_network_preparing_);
        test_voice_now_us += 60000000;
        app.PollWatchNetwork();
        assert(app.watch_network_failed_ && !app.watch_network_preparing_ && board.station_stops == 2);
    }
    std::puts("PASS: driver failure and missing IP leave connecting with an explicit retryable failure");
    {
        Reset();
        Application app;
        AudreamPowerManager::GetInstance().state = AudreamPowerState::kIdle;
        app.StartWatchBootstrap(initial);
        assert(!app.watch_cloud_task_.Active() && clock_updates == 0);
        AudreamPowerManager::GetInstance().state = AudreamPowerState::kActive;
        DeviceTimeService::GetInstance().synchronized = false;
        app.StartWatchBootstrap(initial);
        assert(!app.watch_cloud_task_.Active() && clock_updates == 0);
        DeviceTimeService::GetInstance().synchronized = true;
        app.watch_cloud_retry_after_us_ = 0;
        clock_failure = true;
        app.StartWatchBootstrap(initial);
        assert(!app.watch_cloud_task_.Active() && clock_updates == 1);
    }
    std::puts("PASS: Application respects idle power state and actual TLS clock preflight");
    {
        Reset();
        Application app;
        app.audio_service_.enabled = false;
        app.PollWatchBootstrap();
        AwaitEntry();
        Release();
        Finish(app);
        assert(app.initialized == 0 && !app.watch_cloud_authorization_);
        assert(!app.audio_service_.IsAssistantUserEnabled());
        assert(acknowledgements == 1 && acknowledged_error == "local_disabled");
        assert(!app.watch_cloud_ack_ && app.audio_service_.drains == 1);
    }
    std::puts("PASS: binding activation can finish while local voice permission stays disabled");
    {
        Reset();
        Application app;
        app.PollWatchBootstrap();
        AwaitEntry();
        assert(app.initialized == 0 && clock_updates == 1);
        Release();
        Finish(app);
        assert(app.initialized == 1 && app.watch_cloud_authorization_);
        assert(app.audio_service_.IsAssistantUserEnabled());
        assert(acknowledgements == 1 && acknowledged_error.empty() && !app.watch_cloud_ack_);
        assert(app.audio_service_.drains == 1);
    }
    std::puts("PASS: Application asynchronously loads binding, joins and installs current authorization");
    for (int change = 0; change < 4; ++change) {
        Reset();
        Application app;
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        if (change == 0) {
            app.audio_service_.enabled = false;
            ++app.watch_voice_request_;
            app.watch_cloud_task_.Cancel();
            app.audio_service_.enabled = true;
        } else if (change == 1) {
            persisted.generation = 2;
        } else if (change == 2) {
            const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
            assert(recording && app.audio_session_coordinator_.Complete(recording));
        } else {
            enable_response = false;
        }
        Release();
        Finish(app);
        assert(!app.watch_cloud_authorization_ && app.initialized == 0);
        assert(acknowledgements == (change == 3 ? 1U : 0U));
    }
    std::puts("PASS: local off/on, rebind, recording takeover and cloud disable reject stale authorization");
    {
        Reset();
        Application app;
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        app.watch_deferred_wifi_request_ = std::make_unique<WatchViewRequest>();
        app.watch_cloud_task_.Cancel();
        app.PollWatchBootstrap();
        assert(app.wifi_changes == 0 && AudreamPowerManager::GetInstance().network_leases == 1);
        Release();
        Finish(app);
        assert(app.wifi_changes == 1 && !app.watch_cloud_authorization_);
    }
    std::puts("PASS: deferred Wi-Fi changes wait for actual request cleanup and join");
    {
        Reset();
        Application app;
        persisted_status = BindingStatus::kStorageError;
        app.PollWatchBootstrap();
        assert(!app.watch_cloud_task_.Active());
        assert(app.watch_cloud_retry_after_us_ == test_voice_now_us + 60000000);
        persisted_status = BindingStatus::kBound;
        app.PollWatchBootstrap();
        assert(!app.watch_cloud_task_.Active());
    }
    std::puts("PASS: unavailable binding backs off rather than spinning NVS requests");
    {
        Reset();
        Application app;
        app.protocol_ = std::make_unique<TestProtocol>();
        app.protocol_->close_ready = false;
        app.PollWatchBootstrap();
        assert(app.watch_cloud_retirement_ && app.audio_session_coordinator_.BlocksSleep());
        app.PollWatchBootstrap();
        assert(app.protocol_ && !app.watch_cloud_task_.Active());
        assert(app.audio_session_coordinator_.BlocksSleep());
        app.protocol_->close_ready = true;
        app.PollWatchBootstrap();
        assert(!app.watch_cloud_retirement_ && !app.protocol_);
        AwaitEntry();
        Release();
        Finish(app);
        assert(app.initialized == 1);
    }
    std::puts("PASS: token refresh retains old transport reservation until actual close");
    {
        Reset();
        Application app;
        app.audio_service_.drain_ready = false;
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        Release();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (app.watch_cloud_task_.Active() && std::chrono::steady_clock::now() < deadline) {
            app.PollWatchBootstrap();
            std::this_thread::sleep_for(1ms);
        }
        assert(app.watch_cloud_application_ && app.audio_session_coordinator_.BlocksSleep());
        assert(app.initialized == 0 && acknowledgements == 0);
        app.PollWatchBootstrap();
        assert(app.watch_cloud_application_ && acknowledgements == 0);
        app.audio_service_.drain_ready = true;
        Finish(app);
        assert(!app.audio_session_coordinator_.BlocksSleep() && acknowledgements == 1);
    }
    std::puts("PASS: settings acknowledgement waits for actual audio quiescence");
    {
        Reset();
        Application app;
        activation_mode = "wake_word";
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        Release();
        Finish(app);
        assert(app.initialized == 0 && !app.watch_cloud_authorization_);
        assert(acknowledgements == 1 && acknowledged_error == "unsupported_mode");
    }
    std::puts("PASS: unsupported settings report an error instead of an applied revision");
    {
        Reset();
        Application app;
        app.initialize_ready = false;
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        Release();
        Finish(app);
        assert(app.initialized == 1 && !app.protocol_ && !app.watch_cloud_authorization_);
        assert(acknowledgements == 1 && acknowledged_error == "application_failed");
        assert(!app.watch_cloud_ack_ && !app.audio_session_coordinator_.BlocksSleep());
    }
    std::puts("PASS: failed protocol initialization cannot acknowledge settings success");
    for (const auto failure : {Result::kTransport, Result::kConflict}) {
        Reset();
        Application app;
        acknowledgement_result = failure;
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        Release();
        Finish(app);
        assert(acknowledgements == 1 && !app.watch_cloud_ack_in_flight_);
        if (failure == Result::kConflict) {
            assert(!app.watch_cloud_ack_ && !app.watch_cloud_authorization_);
        } else {
            assert(app.watch_cloud_ack_ && app.watch_cloud_authorization_);
        }
        app.PollWatchBootstrap();
        assert(!app.watch_cloud_task_.Active() && acknowledgements == 1);
    }
    std::puts("PASS: network failures back off and revision conflicts invalidate authorization");
    {
        Reset();
        Application app;
        block_acknowledgement = true;
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        Release();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!acknowledgement_entered && std::chrono::steady_clock::now() < deadline) {
            app.PollWatchBootstrap();
            std::this_thread::sleep_for(1ms);
        }
        assert(acknowledgement_entered && app.watch_cloud_ack_in_flight_);
        const auto reservation = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
        assert(reservation && app.audio_session_coordinator_.Complete(reservation));
        assert(!app.watch_cloud_task_.StopWithin(0));
        assert(AudreamPowerManager::GetInstance().network_leases == 1);
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            release_acknowledgement = true;
            changed.notify_all();
        }
        assert(app.watch_cloud_task_.StopWithin(2000));
        app.PollWatchBootstrap();
        assert(!app.watch_cloud_ack_in_flight_ && !app.watch_cloud_ack_);
        assert(acknowledgements == 0 && AudreamPowerManager::GetInstance().network_leases == 0);
    }
    std::puts("PASS: recording handoff joins acknowledgement cleanup and discards consumed completion");
    for (int change = 0; change < 3; ++change) {
        Reset();
        Application app;
        app.audio_service_.drain_ready = false;
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        Release();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!app.watch_cloud_application_ && std::chrono::steady_clock::now() < deadline) {
            app.PollWatchBootstrap();
            std::this_thread::sleep_for(1ms);
        }
        assert(app.watch_cloud_application_);
        if (change == 0) ++app.watch_voice_request_;
        else if (change == 1) ++persisted.generation;
        else WifiStation::GetInstance().connected = false;
        app.audio_service_.drain_ready = true;
        Finish(app);
        assert(acknowledgements == 0 && app.initialized == 0 && !app.watch_cloud_authorization_);
        assert(!app.audio_session_coordinator_.BlocksSleep());
    }
    std::puts("PASS: settings become stale during drain on local intent, binding or Wi-Fi changes");
    for (int failure = 0; failure < 4; ++failure) {
        Reset();
        Application app;
        if (failure == 0) receipt_load_result = ReceiptStatus::kStorageError;
        else if (failure == 1) receipt_save_result = ReceiptStatus::kStorageError;
        else if (failure == 2) {
            receipt_load_result = ReceiptStatus::kOk;
            persisted_receipt = {2, 2, true, {}};
        } else {
            receipt_load_result = ReceiptStatus::kOk;
            persisted_receipt = {1, 1, false, {}};
        }
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        Release();
        Finish(app);
        assert(!app.watch_cloud_authorization_ && !app.watch_cloud_ack_);
        assert(!app.audio_session_coordinator_.BlocksSleep());
        if (failure < 2) assert(acknowledgements == 1 && acknowledged_error == "application_failed");
        else assert(acknowledgements == 0 && app.initialized == 0 && receipt_saves == 0);
    }
    std::puts("PASS: receipt failures cannot acknowledge success and rollback cannot replace newer settings");
    {
        Reset();
        Application app;
        receipt_load_result = ReceiptStatus::kOk;
        persisted_receipt = {1, 1, true, {}};
        app.audio_service_.enabled = false;
        assert(!app.watch_cloud_authorization_ && !app.protocol_);
        app.StartWatchBootstrap(initial);
        AwaitEntry();
        Release();
        Finish(app);
        assert(!app.audio_service_.enabled && !app.watch_cloud_authorization_ && app.initialized == 0);
        assert(acknowledged_error == "local_disabled" && persisted_receipt.error == "local_disabled");
    }
    std::puts("PASS: restored successful receipt cannot restore tokens or override local permission");
    for (bool enabled : {true, false}) {
        Reset();
        Application app;
        power_mode = "battery_saver";
        enable_response = enabled;
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        assert(acknowledgements == 1 && acknowledged_error.empty());
        assert(AudreamPowerManager::GetInstance().saver);
        assert(persisted_receipt.power_mode == "battery_saver" && persisted_receipt.applied_power_mode == "battery_saver");
        assert(bool(app.watch_cloud_authorization_) == enabled);
        Application reboot;
        AudreamPowerManager::GetInstance().saver = false;
        WifiStation::GetInstance().connected = false;
        reboot.audio_service_.enabled = false;
        reboot.PollWatchPowerPolicy();
        assert(AudreamPowerManager::GetInstance().saver && !reboot.audio_service_.enabled);
        assert(!reboot.protocol_ && !reboot.watch_cloud_authorization_ && !reboot.watch_cloud_task_.Active());
        ++persisted.generation;
        receipt_load_result = ReceiptStatus::kAbsent;
        reboot.PollWatchPowerPolicy();
        assert(!AudreamPowerManager::GetInstance().saver);
    }
    std::puts("PASS: battery saver applies independently of assistant enablement and restores offline only for its owner");
    for (int failure = 0; failure < 3; ++failure) {
        Reset();
        Application app;
        power_mode = failure == 2 ? "always_available" : "battery_saver";
        if (failure == 0) receipt_save_result = ReceiptStatus::kStorageError;
        if (failure == 1) AudreamPowerManager::GetInstance().policy_allowed = false;
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        assert(!AudreamPowerManager::GetInstance().saver && !app.watch_cloud_authorization_);
        assert(acknowledgements == 1 && !acknowledged_error.empty());
    }
    std::puts("PASS: power application and persistence failures cannot claim an applied mode");
    {
        Reset();
        Application app;
        receipt_load_result = ReceiptStatus::kOk;
        persisted_receipt = {2, 1, true, "unsupported_mode", "always_available", "battery_saver"};
        app.PollWatchPowerPolicy();
        assert(AudreamPowerManager::GetInstance().saver);
        persisted_status = BindingStatus::kRemoved;
        app.PollWatchPowerPolicy();
        assert(!AudreamPowerManager::GetInstance().saver);
    }
    std::puts("PASS: failed new settings retain the last applied power policy and removal clears it");
    {
        Reset();
        Application app;
        receipt_load_result = ReceiptStatus::kStorageError;
        app.PollWatchPowerPolicy();
        assert(!app.watch_power_restored_ && !AudreamPowerManager::GetInstance().saver);
        receipt_load_result = ReceiptStatus::kOk;
        persisted_receipt = {1, 1, false, {}, "battery_saver", "battery_saver"};
        app.PollWatchPowerPolicy();
        assert(!app.watch_power_restored_);
        test_voice_now_us += 60000000;
        app.PollWatchPowerPolicy();
        assert(app.watch_power_restored_ && AudreamPowerManager::GetInstance().saver);
    }
    std::puts("PASS: power restore retries failed storage without a network or microphone request");
    {
        Reset();
        Application app;
        receipt_load_result = ReceiptStatus::kOk;
        persisted_receipt = {1, 1, false, {}, "battery_saver", "battery_saver"};
        rebind_during_restore = true;
        app.PollWatchPowerPolicy();
        assert(!app.watch_power_restored_ && !AudreamPowerManager::GetInstance().saver);
        app.PollWatchPowerPolicy();
        assert(app.watch_power_restored_ && !AudreamPowerManager::GetInstance().saver);
    }
    std::puts("PASS: rebind between receipt load and policy restoration rejects the old owner");
    for (int reason = 0; reason < 4; ++reason) {
        Reset();
        Application app;
        app.watch_cloud_required_ = false;
        Bootstrap authorization;
        authorization.settings.enabled = true;
        authorization.settings.activation_mode = "wake_word";
        authorization.settings.wake_word_enabled = true;
        authorization.settings.power_mode = "always_available";
        authorization.access_token = "test-token";
        authorization.websocket_url = "wss://voice.example.test";
        authorization.expires_at_ms = 20000;
        app.watch_cloud_authorization_ = authorization;
        assert(app.watch_wake_lease_.Grant(initial.generation, 1, 10000, 20000));
        app.PollWatchBootstrap();
        assert(app.watch_wake_lease_.Allows(initial.generation, 1, 10000));
        if (reason == 0) app.watch_cloud_authorization_.reset();
        if (reason == 1) app.watch_cloud_authorization_->settings.activation_mode = "push_to_talk";
        if (reason == 2) app.watch_cloud_authorization_->expires_at_ms = 14000;
        if (reason == 3) app.watch_cloud_ack_ = PendingAcknowledgement{};
        app.PollWatchBootstrap();
        assert(!app.watch_wake_lease_.Allows(initial.generation, 1, 10000));
    }
    std::puts("PASS: bootstrap invalidation and pending acknowledgement revoke wake permission");
    {
        Reset();
        Application app;
        activation_mode = "wake_word";
        power_mode = "always_available";
        block_acknowledgement = true;
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!acknowledgement_entered && std::chrono::steady_clock::now() < deadline) {
            app.PollWatchBootstrap();
            std::this_thread::sleep_for(1ms);
        }
        assert(acknowledgement_entered);
        assert(app.audio_service_.wake_preparations == 1 && persisted_receipt.applied_revision == 1);
        app.PollWatchWakeListening();
        assert(!app.audio_service_.wake_running && !app.watch_wake_confirmed_revision_);
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            release_acknowledgement = true;
            changed.notify_all();
        }
        Finish(app);
        assert(app.watch_wake_confirmed_revision_ == 1 && app.watch_always_available_);
        AudreamPowerManager::GetInstance().state = AudreamPowerState::kIdle;
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_running && app.watch_wake_started_);
        assert(app.watch_wake_lease_.Allows(initial.generation, app.audio_session_coordinator_.Generation(), 10000));
        const auto starts = app.audio_service_.wake_starts;
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_starts == starts);
        const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
        assert(recording);
        app.PollWatchWakeListening();
        assert(!app.watch_wake_started_);
        assert(!app.watch_wake_lease_.Allows(initial.generation, recording.generation, 10000));
        assert(app.audio_service_.QuiesceAssistant(0));
        assert(app.audio_session_coordinator_.Fail(recording, true));
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_running && app.audio_service_.wake_starts == starts + 1);
        GetWatchWifiGate().Initialize(false);
        app.PollWatchWakeListening();
        assert(app.watch_wake_retirement_);
        app.audio_service_.drain_ready = false;
        app.PollWatchWakeListening();
        assert(app.watch_wake_retirement_ && app.audio_session_coordinator_.Mode() == AudioSessionMode::kTransitioning);
        app.audio_service_.drain_ready = true;
        app.PollWatchWakeListening();
        assert(!app.watch_wake_retirement_ && !app.audio_service_.wake_running);
        GetWatchWifiGate().Initialize(true);
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_running);
        ++persisted.generation;
        app.PollWatchWakeListening();
        app.PollWatchWakeListening();
        assert(!app.audio_service_.wake_running && !app.watch_wake_started_);
    }
    std::puts("PASS: acknowledged wake settings arm in idle, yield to recording, drain before release and reject rebind");
    for (int failure = 0; failure < 4; ++failure) {
        Reset();
        Application app;
        activation_mode = "wake_word";
        power_mode = "always_available";
        if (failure == 0) Assets::GetInstance().available = false;
        if (failure == 1) app.audio_service_.wake_ready = false;
        if (failure == 2) app.audio_service_.processor_ready = false;
        if (failure == 3) receipt_save_result = ReceiptStatus::kStorageError;
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        app.PollWatchWakeListening();
        assert(!app.watch_cloud_authorization_ && !app.watch_wake_confirmed_revision_ &&
            !app.audio_service_.wake_running && !acknowledged_error.empty());
        assert(Assets::GetInstance().health_begins == 1);
        assert(Assets::GetInstance().models_ready == (failure == 3));
        assert(app.watch_asset_health_active_ == (failure == 3));
    }
    std::puts("PASS: model, endpointing and persistence failures never authorize wake capture");
    {
        Reset();
        Application app;
        activation_mode = "wake_word";
        power_mode = "always_available";
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_running);
        AudreamPowerManager::GetInstance().state = AudreamPowerState::kIdle;
        test_voice_now_us += 60000000;
        entered = release_request = false;
        app.PollWatchBootstrap();
        assert(app.watch_cloud_retirement_ && !app.watch_wake_confirmed_revision_);
        app.audio_service_.drain_ready = false;
        app.PollWatchBootstrap();
        assert(app.watch_cloud_retirement_ && !entered);
        app.audio_service_.drain_ready = true;
        app.PollWatchBootstrap();
        AwaitEntry();
        assert(!app.audio_service_.wake_running && AudreamPowerManager::GetInstance().state == AudreamPowerState::kIdle);
        Release(); Finish(app);
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_running);
    }
    std::puts("PASS: idle refresh drains the detector, renews authorization without waking the screen and rearms");
    for (int reason = 0; reason < 3; ++reason) {
        Reset();
        Application app;
        activation_mode = "wake_word";
        power_mode = "always_available";
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_running);
        if (reason == 0) test_voice_now_us = 3596000000LL;
        if (reason == 1) ++app.watch_voice_request_;
        if (reason == 2) app.audio_service_.enabled = false;
        app.PollWatchWakeListening();
        app.PollWatchWakeListening();
        assert(!app.audio_service_.wake_running && !app.watch_wake_started_);
    }
    std::puts("PASS: expiry, changed local intent and local disable retire an armed detector");
    {
        Reset();
        Application app;
        activation_mode = "wake_word";
        power_mode = "always_available";
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        app.audio_service_.start_ready = false;
        app.PollWatchWakeListening();
        assert(!app.watch_wake_started_ && app.watch_wake_retirement_);
        app.PollWatchWakeListening();
        app.audio_service_.start_ready = true;
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_starts == 1 && !app.audio_service_.wake_running);
        test_voice_now_us += 60000000;
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_running && app.audio_service_.wake_starts == 2);
    }
    std::puts("PASS: failed detector start revokes input and backs off before retry");
    {
        Reset();
        Application app;
        activation_mode = "wake_word";
        power_mode = "always_available";
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        app.PollWatchWakeListening();
        assert(app.audio_service_.wake_running && app.BuildWatchPowerModeSnapshot().can_change);
        app.audio_service_.idle = false;
        assert(app.BuildWatchPowerModeSnapshot().can_change);
        assert(!app.RequestWatchPowerMode(9, initial.generation, "battery_saver"));
        assert(!app.RequestWatchPowerMode(1, initial.generation + 1, "battery_saver"));
        assert(app.RequestWatchPowerMode(1, initial.generation, "battery_saver"));
        assert(!app.BuildWatchPowerModeSnapshot().can_change);
        app.PollWatchBootstrap();
        assert(app.watch_cloud_retirement_ && power_updates == 0);
        app.audio_service_.drain_ready = false;
        app.PollWatchBootstrap();
        assert(power_updates == 0);
        app.audio_service_.drain_ready = true;
        app.audio_service_.idle = true;
        block_acknowledgement = true;
        acknowledgement_entered = false;
        app.PollWatchBootstrap();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!acknowledgement_entered && std::chrono::steady_clock::now() < deadline) {
            app.PollWatchBootstrap();
            std::this_thread::sleep_for(1ms);
        }
        assert(acknowledgement_entered && power_updates == 1);
        assert(!app.audio_service_.wake_running && AudreamPowerManager::GetInstance().saver);
        auto state = app.BuildWatchPowerModeSnapshot();
        assert(state.revision == 2 && state.power_mode == "battery_saver" && state.pending && !state.can_change);
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            release_acknowledgement = true;
            changed.notify_all();
        }
        Finish(app);
        state = app.BuildWatchPowerModeSnapshot();
        assert(state.applied_revision == 2 && !state.pending && state.can_change);
        app.PollWatchWakeListening();
        assert(!app.audio_service_.wake_running && power_updates == 1);
    }
    std::puts("PASS: watch mode selection drains wake input, mutates once, applies and awaits acknowledgement");
    for (const bool committed : {false, true}) {
        Reset();
        Application app;
        enable_response = false;
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        power_update_result = committed ? Result::kTimeout : Result::kConflict;
        commit_power_on_failure = committed;
        assert(app.RequestWatchPowerMode(1, initial.generation, "battery_saver"));
        app.PollWatchBootstrap();
        Finish(app);
        assert(power_updates == 1 && !app.watch_power_change_);
        assert(app.BuildWatchPowerModeSnapshot().power_mode == (committed ? "battery_saver" : "watch_standby"));
        assert(AudreamPowerManager::GetInstance().saver == committed);
        assert(!app.watch_cloud_authorization_);
    }
    std::puts("PASS: conflicting and ambiguous power writes reconcile without mutation retry or enabling voice");
    for (int reason = 0; reason < 4; ++reason) {
        Reset();
        Application app;
        enable_response = false;
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        assert(app.RequestWatchPowerMode(1, initial.generation, "battery_saver"));
        std::optional<AudioSessionTransition> recording;
        if (reason == 0) recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
        if (reason == 1) ++persisted.generation;
        if (reason == 2) GetWatchWifiGate().Initialize(false);
        if (reason == 3) ++app.watch_voice_request_;
        app.PollWatchBootstrap();
        Finish(app);
        assert(!app.watch_power_change_ && power_updates == 0);
        if (recording) app.audio_session_coordinator_.Fail(*recording, true);
    }
    std::puts("PASS: recording, unbind, Wi-Fi disable and changed intent discard unsent power changes");
    {
        Reset();
        Application app;
        enable_response = false;
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        block_power_update = true;
        assert(app.RequestWatchPowerMode(1, initial.generation, "battery_saver"));
        app.PollWatchBootstrap();
        {
            std::unique_lock<std::mutex> lock(request_mutex);
            assert(changed.wait_for(lock, 2s, [] { return power_update_entered.load(); }));
        }
        const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
        app.PollWatchBootstrap();
        assert(AudreamPowerManager::GetInstance().network_leases == 1);
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            release_power_update = true;
            changed.notify_all();
        }
        Finish(app);
        assert(!app.watch_power_change_ && power_updates == 1 && server_revision == 1);
        assert(!AudreamPowerManager::GetInstance().saver);
        app.audio_session_coordinator_.Fail(recording, true);
    }
    std::puts("PASS: recording cancels an in-flight mode update and releases power only after worker join");
    {
        Reset();
        Application app;
        enable_response = false;
        app.StartWatchBootstrap(initial);
        AwaitEntry(); Release(); Finish(app);
        block_power_update = true;
        assert(app.RequestWatchPowerMode(1, initial.generation, "battery_saver"));
        app.PollWatchBootstrap();
        {
            std::unique_lock<std::mutex> lock(request_mutex);
            assert(changed.wait_for(lock, 2s, [] { return power_update_entered.load(); }));
        }
        {
            std::lock_guard<std::mutex> lock(app.watch_voice_mutex_);
            ++app.watch_voice_request_;
        }
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            release_power_update = true;
            changed.notify_all();
        }
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!power_update_guard_checked && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        assert(power_update_guard_checked);
        Finish(app);
        assert(server_revision == 1 && power_updates == 1 && !AudreamPowerManager::GetInstance().saver);
    }
    std::puts("PASS: the worker itself rejects changed local intent before the next application poll");
}
