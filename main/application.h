#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>
#include <optional>

#include "protocol.h"
#include "protocols/assistant_transport_handoff.h"
#include "ota.h"
#include "ota_boot_health.h"
#include "audio_service.h"
#include "audio/assistant_session_lifecycle.h"
#if CONFIG_AUDREAM_WATCH_UI
#include "audio/recording_playback_task.h"
#include "watch/watch_alert_sound.h"
#include "watch/watch_firmware_view_state.h"
#endif
#include "watch/watch_assistant_state.h"
#include "watch/watch_recording_state.h"
#include "watch/watch_recording_library.h"
#include "watch/watch_push_to_talk.h"
#include "watch/watch_voice_capture.h"
#include "watch/watch_speech_interrupt.h"
#include "watch/watch_wake_lease.h"
#include "watch/watch_station_recovery.h"
#include "watch/watch_station_policy.h"
#include "watch/watch_voice_binding.h"
#include "watch/watch_voice_task.h"
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
#include "watch/watch_pairing_controller.h"
#include "watch/watch_pairing_view_state.h"
#endif
#include "watch/watch_power_mode.h"
#include "device_state.h"
#include "device_state_machine.h"
namespace asset_storage { class UpdateSession; }

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_TOGGLE_CAMERA        (1 << 13)
#define MAIN_EVENT_PROTOCOL_RECEIVE     (1 << 14)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

struct WatchViewRequest;

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);
    void ScheduleProtocol(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    /**
     * Toggle camera mode (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CAMERA to be handled in Run()
     */
    void ToggleCameraMode();

    /**
     * Pause camera preview (for taking photos)
     * Returns true if preview was paused, false if no preview was running
     */
    bool PauseCameraPreview();

    /**
     * Resume camera preview (after taking photos)
     * Only resumes if preview was previously paused
     */
    void ResumeCameraPreview();

    /**
     * Close text result and restore camera preview
     * Called when user presses BOOT button to exit listening
     */
    void CloseTextResultAndRestorePreview();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SendText(const std::string& text);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioSessionCoordinator audio_session_coordinator_;
    AssistantSessionLifecycle assistant_session_;
    WatchAssistantState watch_assistant_;
    WatchPushToTalk watch_press_;
    std::atomic<uint64_t> assistant_press_token_{0};
    std::atomic<uint64_t> assistant_capture_deadline_us_{0};
    bool watch_capture_guard_ready_ = false;
    WatchWakeLease watch_wake_lease_;
    bool watch_wake_guard_ready_ = false;
    bool watch_always_available_ = false;
    uint64_t watch_wake_confirmed_revision_ = 0;
    uint64_t watch_wake_confirmed_intent_ = 0;
    bool watch_wake_started_ = false;
    int64_t watch_wake_retry_after_us_ = 0;
    std::optional<AudioSessionTransition> watch_wake_retirement_;
    bool WatchCloudPowerReady() const;
    void PollWatchWakeListening();
    void PollWatchAssetHealth(bool healthy);
    bool watch_voice_save_failed_ = false;
    bool watch_notifications_save_failed_ = false;
    std::mutex watch_notifications_mutex_;
    uint64_t watch_notifications_request_ = 0;
    bool watch_wifi_change_failed_ = false;
    WatchStationRecovery watch_station_recovery_;
    WatchStationPolicy watch_station_policy_;
    bool watch_network_preparing_ = false;
    bool watch_network_failed_ = false;
    uint64_t watch_network_binding_generation_ = 0;
    uint64_t watch_network_voice_intent_ = 0;
    int64_t watch_network_deadline_us_ = 0;
    bool CanPrepareWatchNetwork();
    bool RequestWatchNetwork();
    void CancelWatchNetwork();
    void PollWatchNetwork();
    std::atomic<bool> watch_firmware_busy_{false};
    bool WatchFirmwareBusy() const { return watch_firmware_busy_.load(); }
#if CONFIG_AUDREAM_WATCH_UI
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
    uint64_t watch_model_validation_request_ = 0;
    uint64_t watch_model_validation_generation_ = 0;
    int64_t watch_model_validation_started_ms_ = 0;
    bool watch_model_validation_storage_ = false;
    WatchModelValidationPhase watch_model_validation_phase_ = WatchModelValidationPhase::kIdle;
    void InitializeWatchFirmware(watch_update::Target target = watch_update::Target::kFirmware);
    WatchUpdateBlock WatchFirmwareBlock(bool installing);
    bool AcquireWatchFirmware(watch_update::Operation operation);
    bool ReleaseWatchFirmware(watch_update::Operation operation, bool io_safe);
    void ReleaseWatchFirmwareNetwork();
    void ObserveWatchFirmwareIntent(const WatchViewRequest& request);
    bool HandleWatchFirmwareRequest(const WatchViewRequest& request);
    bool StopWatchFirmware(uint32_t timeout_ms);
    void PollWatchFirmware();
    bool StartWatchModelValidation(uint64_t request_id);
    bool StopWatchModelValidation(uint64_t request_id, bool cancelled, uint32_t timeout_ms);
    void PollWatchModelValidation();
    WatchFirmwareSnapshot BuildWatchFirmwareSnapshot();
#endif
    void PollWatchStationRecovery();
    std::mutex watch_voice_mutex_;
    uint64_t watch_voice_request_ = 0;
    watch_voice::RequestTask watch_cloud_task_;
    static watch_voice::Endpoints WatchVoiceEndpoints();
    static bool PrepareWatchTlsClock();
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    std::unique_ptr<watch_voice::PairingController> watch_pairing_;
    std::mutex watch_pairing_intent_mutex_;
    uint64_t watch_pairing_open_request_ = 0;
    uint64_t watch_pairing_cancelled_open_ = 0;
    uint64_t watch_pairing_attempt_ = 0;
    uint64_t watch_pairing_peer_confirmation_ = 0;
    uint64_t watch_pairing_account_request_ = 0;
    uint64_t watch_pairing_rejected_request_ = 0;
    std::atomic<bool> watch_pairing_revoked_{true};
    std::atomic<bool> watch_pairing_control_only_{false};
    std::atomic<bool> watch_pairing_removal_{false};
    std::atomic<uint64_t> watch_pairing_control_removal_{0};
    std::atomic<bool> watch_pairing_saved_removal_{false};
    std::atomic<uint64_t> watch_pairing_audio_generation_{0};
    bool watch_pairing_storage_ = false;
    bool watch_pairing_network_ = false;
    uint64_t watch_pairing_ble_ = 0;
    void InitializeWatchPairing();
    bool CanOpenWatchPairing(bool control_only = false, bool removing_binding = false, uint64_t control_peer = 0, bool saved_control = false);
    bool AcquireWatchPairing();
    void ReleaseWatchPairing();
    void ObserveWatchPairingIntent(const WatchViewRequest& request);
    bool HandleWatchPairingRequest(const WatchViewRequest& request);
    bool StopWatchPairing(uint32_t timeout_ms);
    WatchPairingSnapshot PollWatchPairing();
#endif
    WatchPowerModeSnapshot watch_power_mode_;
    std::optional<WatchPowerModeChange> watch_power_change_;
    WatchPowerModeSnapshot BuildWatchPowerModeSnapshot();
    bool RequestWatchPowerMode(uint64_t revision, uint64_t binding_generation, const std::string& mode);
    void CancelWatchPowerMode(watch_voice::Result result);
    bool watch_cloud_required_ = false;
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
    void StartWatchBootstrap(const watch_voice::Binding& binding);
    void PollWatchBootstrap();
    void PollWatchSettingsApplication();
    void StartWatchSettingsAcknowledgement();
    void FinishWatchSettingsAcknowledgement(watch_voice::Result status, const watch_voice::Response& response);
    bool FinishWatchPress(uint64_t token);
    WatchVoiceCapture watch_voice_capture_;
    std::unique_ptr<WatchSpeechInterrupt> watch_speech_interrupt_;
    std::vector<int16_t> assistant_capture_prefix_;
    void PollWatchSpeechInterrupt();
    std::atomic<uint64_t> assistant_wake_capture_generation_{0};
    bool assistant_wake_turn_ = false;
    WatchRecordingLibrary watch_library_;
    WatchRecordingDeleteSnapshot watch_deletion_;
    uint64_t watch_library_request_ = 0;
    int64_t watch_library_deadline_us_ = 0;
    void PollWatchLibrary(uint64_t request_id);
    void CancelWatchLibrary();
    AssistantTransportHandoff assistant_transport_handoff_;
    std::atomic<TaskHandle_t> main_task_handle_{nullptr};
    AudioService audio_service_;
#if CONFIG_AUDREAM_WATCH_UI
    std::unique_ptr<RecordingPlaybackTask> watch_playback_;
    std::unique_ptr<WatchAlertSound> watch_alert_sound_;
#endif
    std::unique_ptr<Ota> ota_;
    OtaBootHealth watch_boot_health_;
    bool watch_asset_health_active_ = false;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    bool assistant_response_finished_ = false;
    bool assistant_stop_listening_pending_ = false;
    int64_t assistant_response_deadline_us_ = 0;
    int64_t assistant_conversation_idle_deadline_us_ = 0;
    std::optional<ListeningMode> pending_assistant_listening_;
    std::string pending_assistant_wake_word_;
    int64_t pending_assistant_deadline_us_ = 0;
    bool reset_protocol_pending_ = false;

#if CONFIG_IDF_TARGET_ESP32S3
    // Camera display state.
    class CameraDisplay* camera_display_ = nullptr;
    bool camera_mode_enabled_ = false;
    bool camera_preview_paused_ = false;
#endif

    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleToggleCameraEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void HandleWatchViewRequest(const WatchViewRequest& request);
    void RefreshWatchDisplay();
    WatchAssistantSnapshot BuildWatchAssistantSnapshot();
    WatchRecordingSnapshot BuildWatchRecordingSnapshot();
    bool PrepareAudioOwnership(uint64_t generation, uint32_t timeout_ms);
    bool PrepareRecordingOwnership(uint64_t generation, uint32_t timeout_ms);
    bool AllowsAssistantSession() const;
    bool HasAssistantSession() const;
    void EndWatchAssistantSession();
    void PollWatchAssistantSession();
    bool RequestWatchListening(ListeningMode mode, const std::string& wake_word = "", uint64_t press_token = 0,
                               std::vector<int16_t> prefix = {});
    void StartPendingWatchListening();
    void ContinueWakeWordInvoke(const std::string& wake_word);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
