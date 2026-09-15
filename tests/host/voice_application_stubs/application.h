#pragma once
#include "ota_boot_health.h"

// Compile the actual Application bootstrap methods with controlled peripherals.
// This fixture does not replace an ESP-IDF build of the full Application.
#include "audio/audio_session_coordinator.h"
#include "watch/watch_voice_binding.h"
#include "watch/watch_voice_task.h"
#include "watch/watch_power_mode.h"
#include "watch/watch_station_policy.h"
#include "watch/watch_station_recovery.h"
#include "watch/watch_wake_lease.h"
#include "assets.h"
#include <memory>
#include <optional>

#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
#include "watch/watch_pairing_controller.h"
#include "watch/watch_pairing_view_state.h"
#include "../../../main/watch/watch_view_state.h"
#elif WATCH_FIRMWARE_APPLICATION_TEST
#include "../../../main/watch/watch_view_state.h"
#include "ota.h"
#else
struct WatchViewRequest { uint64_t id = 0; };
#endif
struct TestAudioService {
    std::atomic<bool> enabled{true};
    bool idle = true;
    bool drain_ready = true;
    unsigned drains = 0;
    bool wake_ready = true, processor_ready = true, wake_running = false, start_ready = true;
    unsigned wake_preparations = 0, wake_starts = 0;
    bool IsAssistantUserEnabled() const { return enabled.load(); }
    bool IsIdle() const { return idle; }
    bool IsAssistantCaptureDrained() const { return drain_ready; }
    bool QuiesceAssistant(uint32_t) { ++drains; if (drain_ready) wake_running = false; return drain_ready; }
    bool IsWakeWordRunning() const { return wake_running; }
    uint32_t WakeInputFeedCount() const { return 123; }
    bool PrepareWatchWakeWord(srmodel_list_t*) { ++wake_preparations; return wake_ready; }
    bool PrepareVoiceProcessing(srmodel_list_t*, bool local) { return local && processor_ready; }
    bool ResumeAssistantAudio() { return drain_ready; }
    bool Stop() { return drain_ready; }
    void EnableWakeWordDetection(bool enabled) { if (enabled) ++wake_starts; wake_running = enabled && start_ready; }
};
struct TestAssistantSession { bool IsClosing() const { return false; } };
struct TestProtocol {
    bool close_ready = true;
    bool CloseAudioChannelWithin(uint32_t) { return close_ready; }
};

class Application {
public:
    TestAudioService audio_service_;
    std::atomic<bool> watch_firmware_busy_{false};
    bool WatchFirmwareBusy() const { return watch_firmware_busy_.load(); }
#if WATCH_FIRMWARE_APPLICATION_TEST
    std::unique_ptr<watch_update::Controller> watch_firmware_;
    std::mutex watch_firmware_intent_mutex_;
    uint64_t watch_firmware_request_ = 0, watch_firmware_cancelled_request_ = 0;
    std::atomic<bool> watch_firmware_revoked_{true};
    std::atomic<uint64_t> watch_firmware_generation_{0};
    std::unique_ptr<WatchViewRequest> watch_firmware_preparation_;
    int64_t watch_firmware_deadline_us_ = 0;
    watch_update::Phase watch_firmware_preparation_phase_ = watch_update::Phase::kIdle;
    bool watch_firmware_network_ = false, watch_firmware_storage_ = false;
    bool watch_assets_initialized_ = false;
    std::unique_ptr<asset_storage::UpdateSession> watch_asset_update_;
    uint64_t watch_library_request_ = 0;
    unsigned reboots = 0;
    void Reboot() { ++reboots; }
    void InitializeWatchFirmware(watch_update::Target target = watch_update::Target::kFirmware);
    WatchUpdateBlock WatchFirmwareBlock(bool installing);
    bool AcquireWatchFirmware(watch_update::Operation operation);
    bool ReleaseWatchFirmware(watch_update::Operation operation, bool io_safe);
    void ReleaseWatchFirmwareNetwork();
    void ObserveWatchFirmwareIntent(const WatchViewRequest& request);
    bool HandleWatchFirmwareRequest(const WatchViewRequest& request);
    bool StopWatchFirmware(uint32_t timeout_ms);
    void PollWatchFirmware();
    WatchFirmwareSnapshot BuildWatchFirmwareSnapshot();
    ~Application() {
        if (watch_firmware_preparation_) ReleaseWatchFirmwareNetwork();
        watch_firmware_.reset();
    }
#endif
    AudioSessionCoordinator audio_session_coordinator_;
    TestAssistantSession assistant_session_;
    std::unique_ptr<TestProtocol> protocol_;
    bool reset_protocol_pending_ = false;
    std::mutex watch_voice_mutex_;
    uint64_t watch_voice_request_ = 0;
    watch_voice::RequestTask watch_cloud_task_;
    static watch_voice::Endpoints WatchVoiceEndpoints();
    static bool PrepareWatchTlsClock();
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    std::unique_ptr<watch_voice::PairingController> watch_pairing_;
    std::mutex watch_pairing_intent_mutex_;
    uint64_t watch_pairing_open_request_ = 0, watch_pairing_cancelled_open_ = 0, watch_pairing_attempt_ = 0;
    uint64_t watch_pairing_peer_confirmation_ = 0, watch_pairing_account_request_ = 0, watch_pairing_rejected_request_ = 0;
    std::atomic<bool> watch_pairing_revoked_{true};
    std::atomic<bool> watch_pairing_control_only_{false};
    std::atomic<bool> watch_pairing_removal_{false};
    std::atomic<uint64_t> watch_pairing_control_removal_{0};
    std::atomic<bool> watch_pairing_saved_removal_{false};
    std::atomic<uint64_t> watch_pairing_audio_generation_{0};
    bool watch_pairing_storage_ = false, watch_pairing_network_ = false;
    uint64_t watch_pairing_ble_ = 0;
    uint64_t watch_library_request_ = 0;
    void InitializeWatchPairing();
    bool CanOpenWatchPairing(bool control_only = false, bool removing_binding = false, uint64_t control_peer = 0, bool saved_control = false);
    bool AcquireWatchPairing();
    void ReleaseWatchPairing();
    void ObserveWatchPairingIntent(const WatchViewRequest& request);
    bool HandleWatchPairingRequest(const WatchViewRequest& request);
    bool StopWatchPairing(uint32_t timeout_ms);
    WatchPairingSnapshot PollWatchPairing();
    ~Application() { while (!StopWatchPairing(1000)) {} watch_pairing_.reset(); }
#endif
    WatchPowerModeSnapshot watch_power_mode_;
    WatchStationPolicy watch_station_policy_;
    WatchStationRecovery watch_station_recovery_;
    bool watch_network_preparing_ = false, watch_wifi_change_failed_ = false;
    bool watch_network_failed_ = false;
    uint64_t watch_network_binding_generation_ = 0, watch_network_voice_intent_ = 0;
    int64_t watch_network_deadline_us_ = 0;
    bool CanPrepareWatchNetwork();
    bool RequestWatchNetwork();
    void CancelWatchNetwork();
    void PollWatchNetwork();
    std::optional<WatchPowerModeChange> watch_power_change_;
    WatchPowerModeSnapshot BuildWatchPowerModeSnapshot();
    bool RequestWatchPowerMode(uint64_t revision, uint64_t binding_generation, const std::string& mode);
    void CancelWatchPowerMode(watch_voice::Result result);
    bool watch_cloud_required_ = true;
    WatchWakeLease watch_wake_lease_;
    bool watch_wake_guard_ready_ = true;
    bool watch_asset_health_active_ = false;
    OtaBootHealth watch_boot_health_;
    bool watch_always_available_ = false;
    uint64_t watch_wake_confirmed_revision_ = 0, watch_wake_confirmed_intent_ = 0;
    bool watch_wake_started_ = false;
    int64_t watch_wake_retry_after_us_ = 0;
    std::optional<AudioSessionTransition> watch_wake_retirement_;
    bool WatchCloudPowerReady() const;
    void PollWatchWakeListening();
    void PollWatchAssetHealth(bool healthy);
    bool has_assistant_session = false;
    bool HasAssistantSession() const { return has_assistant_session; }
    std::optional<int> pending_assistant_listening_;
    bool watch_power_restored_ = false;
    uint64_t watch_power_binding_generation_ = 0;
    int64_t watch_power_retry_after_us_ = 0;
    void PollWatchPowerPolicy();
    std::optional<watch_voice::Bootstrap> watch_cloud_authorization_;
    std::string watch_cloud_device_id_;
    uint64_t watch_cloud_binding_generation_ = 0;
    uint64_t watch_cloud_request_audio_generation_ = 0;
    uint64_t watch_cloud_request_voice_revision_ = 0;
    int64_t watch_cloud_retry_after_us_ = 0;
    std::optional<AudioSessionTransition> watch_cloud_retirement_;
    std::optional<watch_voice::Bootstrap> watch_cloud_application_;
    std::optional<AudioSessionTransition> watch_cloud_application_transition_;
    std::optional<watch_voice::PendingAcknowledgement> watch_cloud_ack_;
    bool watch_cloud_ack_in_flight_ = false;
    int64_t watch_cloud_ack_retry_after_us_ = 0;
    std::unique_ptr<WatchViewRequest> watch_deferred_wifi_request_;
    unsigned initialized = 0;
    bool initialize_ready = true;
    unsigned wifi_changes = 0;
    void StartWatchBootstrap(const watch_voice::Binding& binding);
    void PollWatchBootstrap();
    void PollWatchSettingsApplication();
    void StartWatchSettingsAcknowledgement();
    void FinishWatchSettingsAcknowledgement(watch_voice::Result status, const watch_voice::Response& response);
    void InitializeProtocol() {
        ++initialized;
        if (initialize_ready) protocol_ = std::make_unique<TestProtocol>();
    }
    void HandleWatchViewRequest(const WatchViewRequest&) { ++wifi_changes; }
};
