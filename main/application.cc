#include "application.h"
#include "watch/watch_voice_settings.h"
#include "watch/watch_notification_settings.h"
#include "watch/watch_voice_binding.h"
#include "watch/watch_wifi_settings.h"
#include "watch/watch_wifi_gate.h"
#include "storage/temporary_ap_transfer_service.h"
#include "board.h"
#include "display.h"
#include "lcd_display.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "camera_display.h"
#include "esp_camera.h"
#endif
#include "esp_lvgl_port.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "firmware_release_identity.h"
#include "settings.h"
#include "system/audream_power_manager.h"
#include "audio/local_recording_service.h"
#include "storage/sd_card_storage.h"
#include "watch/watch_time_runtime.h"
#include "watch/watch_view_state.h"
#if CONFIG_AUDREAM_WATCH_ANCS
#include "watch/watch_ancs_client.h"
#endif
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
#include "watch/watch_android_gatt_server.h"
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
#include "watch/watch_pairing_gatt_server.h"
#endif
#include "blufi.h"
#include "wifi_manager.h"

#include <cstring>
#include <chrono>
#include <algorithm>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


Application::Application()
    : assistant_session_(audio_session_coordinator_,
          [this](uint32_t timeout_ms) { return !protocol_ || protocol_->CloseAudioChannelWithin(timeout_ms); },
          [this](uint32_t timeout_ms) { return audio_service_.QuiesceAssistant(timeout_ms); },
          [this]() { return audio_service_.ResumeAssistantAudio(); }),
      assistant_transport_handoff_(
          [this](std::function<void()> callback) { Schedule(std::move(callback)); },
          [this]() { return main_task_handle_.load() == xTaskGetCurrentTaskHandle(); },
          [this](uint64_t generation) {
              return audio_session_coordinator_.HoldsExclusiveResources(generation);
          },
          [this](uint32_t timeout_ms) {
#if CONFIG_AUDREAM_WATCH_UI
              const auto update_started = std::chrono::steady_clock::now();
              if (!StopWatchFirmware(timeout_ms)) return false;
              const auto update_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - update_started).count();
              timeout_ms = static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(timeout_ms) - update_elapsed));
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
              const auto started = std::chrono::steady_clock::now();
              if (!StopWatchPairing(timeout_ms)) return false;
              const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started).count();
              timeout_ms = static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(timeout_ms) - elapsed));
#endif
              CancelWatchLibrary();
              if (assistant_session_.RetireIfPreempted()) {
                  watch_press_.Retire(assistant_press_token_.exchange(0));
                  assistant_capture_deadline_us_ = 0;
                  pending_assistant_listening_.reset();
                  pending_assistant_wake_word_.clear();
                  assistant_response_finished_ = false;
                  assistant_stop_listening_pending_ = false;
                  assistant_response_deadline_us_ = 0;
                  assistant_conversation_idle_deadline_us_ = 0;
                  SetDeviceState(kDeviceStateIdle);
              }
              return !protocol_ || protocol_->CloseAudioChannelWithin(timeout_ms);
          }) {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    // Captured Application members must outlive all network worker cleanup.
#if CONFIG_AUDREAM_WATCH_UI
    if (watch_model_validation_generation_) StopWatchModelValidation(0, true, 3000);
    if (watch_firmware_preparation_) {
        watch_firmware_preparation_.reset();
        ReleaseWatchFirmwareNetwork();
    }
    {
        std::lock_guard<std::mutex> lock(watch_firmware_intent_mutex_);
        watch_firmware_revoked_.store(true);
        watch_firmware_.reset();
    }
#endif
    while (!watch_cloud_task_.StopWithin(1000)) {}
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    while (!StopWatchPairing(1000)) {}
    {
        std::lock_guard<std::mutex> lock(watch_pairing_intent_mutex_);
        watch_pairing_.reset();
    }
#endif
#if CONFIG_AUDREAM_WATCH_UI
    // Output completion hooks may still schedule work through event_group_.
    watch_alert_sound_.reset();
    watch_playback_.reset();
#endif
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    if (Board::GetInstance().GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
        if ((state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) &&
            !audio_service_.IsAssistantUserEnabled()) return false;
        if ((state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) &&
            main_task_handle_.load() != xTaskGetCurrentTaskHandle()) {
            ESP_LOGW(TAG, "Assistant state changes must be scheduled on the application task");
            return false;
        }
        if (!state_machine_.CanTransitionTo(state)) return false;
        if (state == kDeviceStateConnecting) {
            if (!assistant_session_.IsActive(assistant_session_.Token()) && !assistant_session_.Begin()) return false;
            if (!assistant_session_.Update(AudioSessionMode::kAssistantConnecting, assistant_session_.Token())) return false;
        } else if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            const auto mode = state == kDeviceStateListening ? AudioSessionMode::kAssistantListening
                                                            : AudioSessionMode::kAssistantSpeaking;
            if (!assistant_session_.Update(mode, assistant_session_.Token())) return false;
        }
    }
    return state_machine_.TransitionTo(state);
}

bool Application::AllowsAssistantSession() const {
    return Board::GetInstance().GetRuntimeProfile() != BoardRuntimeProfile::kWatchAssistantRecorder ||
        (GetWatchWifiGate().Enabled() && audio_service_.IsAssistantUserEnabled() && audio_session_coordinator_.AllowsAssistantResources());
}

bool Application::HasAssistantSession() const {
    return Board::GetInstance().GetRuntimeProfile() != BoardRuntimeProfile::kWatchAssistantRecorder ||
        assistant_session_.IsActive(assistant_session_.Token());
}

void Application::EndWatchAssistantSession() {
    if (watch_speech_interrupt_) watch_speech_interrupt_->Cancel();
    std::fill(assistant_capture_prefix_.begin(), assistant_capture_prefix_.end(), 0);
    assistant_capture_prefix_.clear();
    assistant_wake_capture_generation_ = 0;
    watch_voice_capture_.Cancel();
    assistant_wake_turn_ = false;
    const auto press = assistant_press_token_.load();
    if (press) watch_press_.Retire(press);
    pending_assistant_listening_.reset();
    pending_assistant_wake_word_.clear();
    assistant_response_finished_ = false;
    assistant_stop_listening_pending_ = false;
    assistant_response_deadline_us_ = 0;
    assistant_conversation_idle_deadline_us_ = 0;
    assistant_session_.Stop(assistant_session_.Token());
    assistant_press_token_ = 0;
    assistant_capture_deadline_us_ = 0;
    SetDeviceState(kDeviceStateIdle);
}

bool Application::FinishWatchPress(uint64_t token) {
    if (!token || token != assistant_press_token_) return true;
    const auto result = watch_press_.Finish(token, esp_timer_get_time() / 1000);
    if (result == WatchPressCompletion::kTimeout) {
        watch_assistant_.Fail(WatchAssistantError::kTimeout);
        EndWatchAssistantSession();
    } else if (result == WatchPressCompletion::kCancel) {
        EndWatchAssistantSession();
    } else if (result == WatchPressCompletion::kSend) {
        HandleStopListeningEvent();
        return HasAssistantSession();
    }
    return true;
}

void Application::PollWatchAssistantSession() {
    if ((!audio_service_.IsAssistantUserEnabled() || !GetWatchWifiGate().Enabled()) &&
        (assistant_session_.Token() || pending_assistant_listening_))
        EndWatchAssistantSession();
    const auto press = assistant_press_token_.load();
    if (press && !watch_press_.Held(press, esp_timer_get_time() / 1000)) FinishWatchPress(press);
    const auto wake_capture = assistant_wake_capture_generation_.load();
    if (wake_capture) {
        if (!assistant_session_.IsActive(wake_capture)) {
            assistant_wake_capture_generation_ = 0;
            watch_voice_capture_.Cancel();
        } else {
            const auto result = watch_voice_capture_.Poll(wake_capture, esp_timer_get_time() / 1000);
            if (result == WatchVoiceCaptureResult::kSend) {
                // Keep the retired token in the input guard until session cleanup.
                HandleStopListeningEvent();
            } else if (result == WatchVoiceCaptureResult::kCancel) {
                EndWatchAssistantSession();
            }
        }
    }
    const auto capture_deadline = assistant_capture_deadline_us_.load();
    if (capture_deadline && static_cast<uint64_t>(esp_timer_get_time()) >= capture_deadline &&
        GetDeviceState() == kDeviceStateListening) {
        ESP_LOGI(TAG, "Assistant capture reached its time limit; sending accepted audio");
        HandleStopListeningEvent();
    }
    if (reset_protocol_pending_) {
        if (assistant_session_.IsClosing()) assistant_session_.Poll();
        assistant_session_.RetireIfPreempted();
        if (assistant_session_.Token() == 0 && (!protocol_ || protocol_->CloseAudioChannelWithin(0))) {
            protocol_.reset();
            reset_protocol_pending_ = false;
        }
        return;
    }
    if (assistant_session_.RetireIfPreempted()) {
        if (watch_speech_interrupt_) watch_speech_interrupt_->Cancel();
        std::fill(assistant_capture_prefix_.begin(), assistant_capture_prefix_.end(), 0);
        assistant_capture_prefix_.clear();
        watch_press_.Retire(assistant_press_token_.exchange(0));
        assistant_capture_deadline_us_ = 0;
        pending_assistant_listening_.reset();
        pending_assistant_wake_word_.clear();
        assistant_response_finished_ = false;
        assistant_stop_listening_pending_ = false;
        assistant_response_deadline_us_ = 0;
        assistant_conversation_idle_deadline_us_ = 0;
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (assistant_session_.IsClosing()) {
        assistant_session_.Poll();
        StartPendingWatchListening();
        return;
    }
    if (assistant_session_.IsTurning()) {
        StartPendingWatchListening();
        return;
    }
    if (!HasAssistantSession()) return;
    PollWatchSpeechInterrupt();
    if (!HasAssistantSession()) return;
    if (assistant_conversation_idle_deadline_us_ && esp_timer_get_time() >= assistant_conversation_idle_deadline_us_) {
        EndWatchAssistantSession();
        return;
    }
    if (assistant_response_deadline_us_ != 0 && esp_timer_get_time() >= assistant_response_deadline_us_) {
        ESP_LOGW(TAG, "Assistant response timed out; closing the session");
        watch_assistant_.Fail(WatchAssistantError::kTimeout);
        EndWatchAssistantSession();
        return;
    }
    if (assistant_stop_listening_pending_ && audio_service_.IsAssistantCaptureDrained()) {
        assistant_stop_listening_pending_ = false;
        if (protocol_) protocol_->SendStopListening();
    }
    if (assistant_response_finished_ && audio_service_.IsAssistantPlaybackDrained()) {
        const bool keep_conversation = !assistant_wake_turn_ && listening_mode_ == kListeningModeManualStop &&
            protocol_ && protocol_->SupportsConversationTurns();
        if (keep_conversation) {
            if (!protocol_->SuspendIncomingTurn()) {
                EndWatchAssistantSession();
                return;
            }
            if (!audio_service_.QuiesceAssistant(0)) return;
        }
        assistant_response_finished_ = false;
        assistant_response_deadline_us_ = 0;
        if (listening_mode_ == kListeningModeManualStop) {
            if (keep_conversation) {
                watch_press_.Retire(assistant_press_token_.exchange(0));
                assistant_conversation_idle_deadline_us_ = esp_timer_get_time() + 30000000LL;
            } else {
                EndWatchAssistantSession();
            }
        } else {
            SetListeningMode(listening_mode_);
        }
    }
}


void Application::StartPendingWatchListening() {
    if (!pending_assistant_listening_) return;
    const auto press = assistant_press_token_.load();
    if (press && !watch_press_.Held(press, esp_timer_get_time() / 1000)) {
        FinishWatchPress(press);
        return;
    }
    if (esp_timer_get_time() >= pending_assistant_deadline_us_ ||
        (!assistant_session_.IsClosing() && !AllowsAssistantSession())) {
        pending_assistant_listening_.reset();
        pending_assistant_wake_word_.clear();
        ESP_LOGW(TAG, "Pending assistant turn expired or was preempted");
        watch_assistant_.Fail(WatchAssistantError::kTimeout);
        EndWatchAssistantSession();
        return;
    }
    if (assistant_session_.IsClosing()) return;
    if (assistant_session_.IsTurning() && !assistant_session_.PollNextTurn(0)) {
        if (!assistant_session_.IsTurning()) {
            watch_assistant_.Fail(WatchAssistantError::kAudio);
            EndWatchAssistantSession();
        }
        return;
    }
    const auto mode = *pending_assistant_listening_;
    const auto wake_word = std::move(pending_assistant_wake_word_);
    pending_assistant_listening_.reset();
    if (!protocol_ || !SetDeviceState(kDeviceStateConnecting)) return;
    const auto token = assistant_session_.Token();
    srmodel_list_t* assistant_models = nullptr;
#if CONFIG_USE_AUDIO_PROCESSOR
    if (!Assets::GetInstance().LoadAssistantModels(assistant_models)) {
        ESP_LOGE(TAG, "Assistant model preparation failed before connection");
        watch_assistant_.Fail(WatchAssistantError::kAudio);
        EndWatchAssistantSession();
        return;
    }
#endif
    if (!audio_service_.PrepareVoiceProcessing(assistant_models, assistant_wake_turn_)) {
        ESP_LOGE(TAG, "Assistant audio preparation failed before connection");
        watch_assistant_.Fail(WatchAssistantError::kAudio);
        EndWatchAssistantSession();
        return;
    }
    RefreshWatchDisplay();
    Schedule([this, token, mode, wake_word]() {
        if (!assistant_session_.IsActive(token)) return;
        // Wake detector audio is not uploaded. A speech interruption instead
        // carries only its checked post-AEC prefix into the new explicit turn.
        play_popup_on_listening_ = !wake_word.empty();
        ContinueOpenAudioChannel(mode);
    });
}

bool Application::PrepareAudioOwnership(uint64_t generation, uint32_t timeout_ms) {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (!audio_session_coordinator_.HoldsExclusiveResources(generation)) return false;
    watch_pairing_revoked_.store(true);
#endif
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (!watch_cloud_task_.StopWithin(timeout_ms)) return false;
    const auto transport_remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (!assistant_transport_handoff_.CloseWithin(generation,
        static_cast<uint32_t>(std::max<int64_t>(0, transport_remaining)))) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (!audio_service_.QuiesceAssistant(static_cast<uint32_t>(std::max<int64_t>(0, remaining)))) return false;
    return audio_session_coordinator_.HoldsExclusiveResources(generation);
}

bool Application::PrepareRecordingOwnership(uint64_t generation, uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (!TemporaryAPTransferService::GetInstance().StopWithin(timeout_ms)) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return PrepareAudioOwnership(generation, static_cast<uint32_t>(std::max<int64_t>(0, remaining)));
}

void Application::PollWatchStationRecovery() {
    if (!watch_station_recovery_.Pending()) return;
    auto& gate = GetWatchWifiGate();
    auto& board = Board::GetInstance();
    const bool ready = !gate.TransferActive() && audio_service_.IsIdle() &&
        !board.GetWatchRecordingControlStatus().starting && !reset_protocol_pending_ &&
        !assistant_session_.IsClosing();
    const auto result = watch_station_recovery_.Poll(esp_timer_get_time() / 1000,
        gate.Enabled() && gate.StationAllowed(), ready, audio_session_coordinator_, [&board]() {
            auto& power = AudreamPowerManager::GetInstance();
            if (!power.BeginStorageRead()) return false;
            const bool started = board.RestoreWatchStation();
            power.EndStorageRead();
            return started;
        });
    if (result == WatchStationRecoveryResult::kFailed) {
        watch_wifi_change_failed_ = true;
        ESP_LOGW(TAG, "Watch station recovery could not start after three attempts");
    }
}

void Application::Initialize() {
    main_task_handle_ = xTaskGetCurrentTaskHandle();
    auto& board = Board::GetInstance();
    bool watch_boot_ready = true;
    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
        const int enabled = ReadWatchVoiceEnabled();
#if CONFIG_AUDREAM_WATCH_ANCS
        const int notifications_enabled = ReadWatchNotificationsEnabled();
        watch_notifications_save_failed_ = notifications_enabled < 0;
        WatchAncsClient::Get().SetEnabled(notifications_enabled == 1);
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
        // Restore the notification identity without starting voice or Wi-Fi.
        if (notifications_enabled == 1 && AudreamPowerManager::GetInstance().BeginStorageRead()) {
            watch_voice::WithBoundIdentity([](std::string_view, uint64_t) { return true; });
            AudreamPowerManager::GetInstance().EndStorageRead();
        }
        WatchAndroidGattServer::Get().SetEnabled(notifications_enabled == 1);
#endif
#endif
        const int wifi_enabled = ReadWatchWifiEnabled();
        GetWatchWifiGate().Initialize(wifi_enabled == 1);
        watch_wifi_change_failed_ = wifi_enabled < 0;
        if (wifi_enabled < 0) ESP_LOGE(TAG, "Watch WiFi preference could not be loaded; WiFi remains disabled");
        audio_service_.SetAssistantUserEnabled(enabled == 1);
        watch_voice_save_failed_ = enabled < 0;
        if (enabled < 0) ESP_LOGE(TAG, "Watch assistant preference could not be loaded; assistant remains disabled");
        audio_service_.SetSessionCoordinator(&audio_session_coordinator_);
        watch_wake_guard_ready_ = audio_service_.SetWakeWordGuard([this]() {
            return GetWatchWifiGate().Enabled() && !GetWatchWifiGate().TransferActive() &&
                WifiManager::GetInstance().IsConnected() &&
                audio_session_coordinator_.Mode() == AudioSessionMode::kIdle &&
                watch_wake_lease_.Allows(watch_voice::ActiveBindingGeneration(),
                    audio_session_coordinator_.Generation(), esp_timer_get_time() / 1000);
        });
        if (!watch_wake_guard_ready_) {
            audio_service_.SetAssistantUserEnabled(false);
            ESP_LOGE(TAG, "Watch wake-word guard could not be installed; assistant remains disabled");
        }
        watch_capture_guard_ready_ = audio_service_.SetAssistantCaptureGuard([this]() {
            if (!GetWatchWifiGate().Enabled()) return false;
            const auto now = static_cast<uint64_t>(esp_timer_get_time());
            const auto wake_capture = assistant_wake_capture_generation_.load();
            if (wake_capture && !watch_voice_capture_.AllowsInput(wake_capture, now / 1000)) return false;
            const auto deadline = assistant_capture_deadline_us_.load();
            if (deadline && now >= deadline) return false;
            const auto token = assistant_press_token_.load();
            return !token || watch_press_.AllowsInput(token, now / 1000);
        });
        if (!watch_capture_guard_ready_) {
            watch_assistant_.Fail(WatchAssistantError::kAudio);
            ESP_LOGE(TAG, "Watch assistant capture guard could not be installed");
        }
        if (!LocalRecordingService::GetInstance().SetAssistantHandoff(
                [this](uint64_t generation, uint32_t timeout_ms) {
                    return PrepareRecordingOwnership(generation, timeout_ms);
                })) {
            ESP_LOGE(TAG, "Failed to register recording ownership handoff");
        }
        if (!TemporaryAPTransferService::GetInstance().SetAudioOwnership(audio_session_coordinator_,
                [this](uint64_t generation, uint32_t timeout_ms) { return PrepareAudioOwnership(generation, timeout_ms); },
                [this]() { Schedule([this]() { audio_service_.ResumeAssistantAudio(); }); },
                [this]() { Schedule([this]() { watch_station_recovery_.Request(); }); })) {
            ESP_LOGE(TAG, "Failed to register transfer ownership handoff");
        }
        AudreamPowerManager::GetInstance().SetAudioSessionCoordinator(
            &audio_session_coordinator_, [this]() {
                return PrepareAudioOwnership(audio_session_coordinator_.Generation(), 3000);
            },
            [this]() { audio_service_.ResumeAssistantAudio(); });
        if (!GetWatchTimeService().Initialize(ReadWatchClock())) {
            ESP_LOGE(TAG, "Watch time state could not be restored; sleep remains inhibited");
        }
        AudreamPowerManager::GetInstance().SetSleepPreparationCallbacks(
            []() {
                uint64_t delay_ms = 0;
                return GetWatchTimeService().PrepareForSleep(ReadWatchClock(), delay_ms);
            },
            [](uint64_t& delay_ms) {
                return GetWatchTimeService().SleepWakeDelay(ReadWatchClock(), delay_ms);
            },
            []() {
                GetWatchTimeService().CancelSleep();
            });
    }
    SetDeviceState(kDeviceStateStarting);
    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
        // Timekeeping must continue even if display or assistant audio startup
        // fails. The clock tick is consumed by the normal main event loop.
        const esp_err_t result = esp_timer_start_periodic(clock_timer_handle_, 1000000);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Watch clock timer initialization failed: %s", esp_err_to_name(result));
            watch_boot_ready = false;
        }
    }

    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kRecordingOnly) {
        ESP_LOGI(TAG, "Starting Audream recording-only mode");
        auto codec = board.GetAudioCodec();
        audio_service_.InitializeForLocalRecording(codec);
        if (!audio_service_.StartForLocalRecording()) {
            ESP_LOGE(TAG, "Recording audio initialization failed");
            board.StartNetwork();
            return;
        }
        board.StartNetwork();
        SetDeviceState(kDeviceStateActivating);
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    // Setup the display
    auto display = board.GetDisplay();
    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
#if CONFIG_AUDREAM_WATCH_UI
        InitializeWatchFirmware();
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        InitializeWatchPairing();
#endif
#if CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING && defined(CONFIG_BT_BLUEDROID_ENABLED)
        // Start the BLE host before the watch UI and audio service consume the
        // remaining internal RAM. Board::StartNetwork() reuses this session.
        const esp_err_t ble_start_result = Blufi::GetInstance().init(false);
        if (ble_start_result != ESP_OK) {
            ESP_LOGE(TAG, "Early watch BLE initialization failed: %s", esp_err_to_name(ble_start_result));
            watch_boot_ready = false;
        }
#endif
        if (!display->SetupWatchUI([this](const WatchViewRequest& request) {
            // Intent fencing is immediate even when the owner is in a handshake.
#if CONFIG_AUDREAM_WATCH_UI
            ObserveWatchFirmwareIntent(request);
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
            ObserveWatchPairingIntent(request);
#endif
            if (request.action == WatchViewAction::kAssistantPress) {
                watch_press_.Press(request.press_token, esp_timer_get_time() / 1000);
            } else if (request.action == WatchViewAction::kAssistantRelease || request.action == WatchViewAction::kAssistantCancelPress) {
                watch_press_.Release(request.press_token, request.action == WatchViewAction::kAssistantCancelPress);
            } else if (request.action == WatchViewAction::kAssistantEnd) {
                watch_press_.CancelPending();
                assistant_capture_deadline_us_ = 1;
            } else if (request.action == WatchViewAction::kSetWifiEnabled) {
                watch_wake_lease_.Revoke();
                watch_cloud_task_.Cancel();
                GetWatchWifiGate().Observe(request.id, request.enabled);
                if (!request.enabled) watch_press_.CancelPending();
            } else if (request.action == WatchViewAction::kSetNotificationsEnabled) {
                std::lock_guard<std::mutex> lock(watch_notifications_mutex_);
                watch_notifications_request_ = request.id;
#if CONFIG_AUDREAM_WATCH_ANCS
                if (!request.enabled) WatchAncsClient::Get().DisableCollection();
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
                if (!request.enabled) WatchAndroidGattServer::Get().DisableCollection();
#endif
#endif
            } else if (request.action == WatchViewAction::kSetVoiceEnabled) {
                watch_wake_lease_.Revoke();
                watch_cloud_task_.Cancel();
                std::lock_guard<std::mutex> lock(watch_voice_mutex_);
                watch_voice_request_ = request.id;
                if (!request.enabled) {
                    audio_service_.SetAssistantUserEnabled(false);
                    watch_press_.CancelPending();
                }
            }
            audio_service_.NotifyAssistantCaptureIntentChanged();
            Schedule([this, request]() { HandleWatchViewRequest(request); });
        })) {
            ESP_LOGE(TAG, "Watch display initialization failed");
            watch_boot_ready = false;
        }
        RefreshWatchDisplay();
    } else {
        display->SetupUI();
    }
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);

    AudioServiceCallbacks callbacks;
    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
        watch_speech_interrupt_ = std::make_unique<WatchSpeechInterrupt>();
        callbacks.on_speech_monitor = [this](uint64_t generation, const int16_t* pcm, size_t count, bool speech) {
            if (watch_speech_interrupt_->Feed(generation, pcm, count, speech, esp_timer_get_time() / 1000)) {
                ESP_LOGI(TAG, "Assistant speech interruption qualified: generation=%llu",
                    static_cast<unsigned long long>(generation));
                xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
            }
        };
    }
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_guarded_wake_word_detected = [this](const std::string& word, uint64_t generation) {
        Schedule([this, word, generation]() {
            if (!watch_wake_guard_ready_ || !audio_service_.IsWakeWordGenerationCurrent(generation)) return;
            if (RequestWatchListening(kListeningModeManualStop, word)) watch_wake_lease_.Revoke();
        });
    };
    callbacks.on_vad_change = [this](bool speaking) {
        const auto generation = assistant_wake_capture_generation_.load();
        if (generation) watch_voice_capture_.Vad(generation, speaking, esp_timer_get_time() / 1000);
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);
    if (!audio_service_.Start()) {
        ESP_LOGE(TAG, "Assistant audio initialization failed");
        return;
    }
#if CONFIG_AUDREAM_WATCH_UI
    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder && codec->SupportsWatchPlayback()) {
        RecordingPlaybackTask::Hooks hooks;
        hooks.acquire_storage = [] { return AudreamPowerManager::GetInstance().BeginStorageRead(); };
        hooks.release_storage = [] { AudreamPowerManager::GetInstance().EndStorageRead(); };
        hooks.prepare_output = [this](uint64_t generation) { return audio_service_.PrepareLocalPlaybackOutput(generation); };
        hooks.write_output = [this](uint64_t generation, const std::vector<int16_t>& pcm) {
            return audio_service_.WriteLocalPlaybackOutput(generation, pcm);
        };
        hooks.pause_output = [this](uint64_t generation) { return audio_service_.PauseLocalPlaybackOutput(generation); };
        hooks.close_output = [this](uint64_t generation) { return audio_service_.CloseLocalPlaybackOutput(generation); };
        hooks.set_volume = [this](uint64_t generation, int volume) { return audio_service_.SetLocalPlaybackVolume(generation, volume); };
        hooks.released = [this] { Schedule([this] { audio_service_.ResumeAssistantAudio(); RefreshWatchDisplay(); }); };
        WatchAlertSound::Hooks alert_hooks{hooks.prepare_output, hooks.write_output, hooks.close_output, hooks.released};
        watch_alert_sound_ = std::make_unique<WatchAlertSound>(audio_session_coordinator_, std::move(alert_hooks), codec->output_sample_rate());
        watch_playback_ = std::make_unique<RecordingPlaybackTask>(audio_session_coordinator_, std::move(hooks), codec->output_sample_rate());
        if (!LocalRecordingService::GetInstance().SetPlaybackHandoff([this](uint64_t generation, uint32_t timeout_ms) {
                const auto started = esp_timer_get_time();
                const bool alert_stopped = !watch_alert_sound_ || watch_alert_sound_->StopForRecording(generation, timeout_ms);
                const auto elapsed_ms = static_cast<uint64_t>((esp_timer_get_time() - started) / 1000);
                const auto remaining_ms = elapsed_ms < timeout_ms ? timeout_ms - static_cast<uint32_t>(elapsed_ms) : 0;
                const bool playback_stopped = watch_playback_ && watch_playback_->StopForRecording(generation, remaining_ms);
                return alert_stopped && playback_stopped;
            })) {
            watch_playback_.reset();
            watch_alert_sound_.reset();
            ESP_LOGE(TAG, "Playback is disabled because recording handoff registration failed");
            watch_boot_ready = false;
        }
    }
#endif

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });
    if (UsesOfflineWatchStartup(board.GetRuntimeProfile())) {
        SetDeviceState(kDeviceStateIdle);
        ESP_LOGI(TAG, "Offline watch ready; assistant network configuration is optional at startup");
    }

    // Start the clock timer to update the status bar
    if (board.GetRuntimeProfile() != BoardRuntimeProfile::kWatchAssistantRecorder) {
        esp_timer_start_periodic(clock_timer_handle_, 1000000);
    }

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
            if (event == NetworkEvent::Connected) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
            } else if (event == NetworkEvent::Disconnected || event == NetworkEvent::Scanning) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
            }
            return;
        }
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

#if CONFIG_AUDREAM_WATCH_UI
    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder &&
        !Assets::GetInstance().InitializePartition()) {
        ESP_LOGE(TAG, "Watch assets initialization failed after network startup");
        watch_boot_ready = false;
    }
#endif

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

#if CONFIG_IDF_TARGET_ESP32S3
    // Start camera preview only on boards that provide a camera.
    if (board.GetCamera() != nullptr) {
        ESP_LOGI(TAG, "Starting camera preview on main screen");
        auto lcd_display = dynamic_cast<LcdDisplay*>(display);
        if (lcd_display != nullptr && lvgl_port_lock(0)) {
            lv_obj_t* preview_img = lcd_display->GetPreviewImage();

            if (preview_img != nullptr) {
                camera_display_ = new CameraDisplay();
                camera_display_->SetCameraImage(preview_img);

                if (camera_display_->Start()) {
                    ESP_LOGI(TAG, "Camera preview started successfully");
                } else {
                    ESP_LOGE(TAG, "Failed to start camera preview");
                    delete camera_display_;
                    camera_display_ = nullptr;
                }
            }

            lvgl_port_unlock();
        }
    } else {
        ESP_LOGI(TAG, "Board does not have camera, skipping camera preview");
    }
#endif
    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
        watch_boot_ready = watch_boot_ready && watch_wake_guard_ready_ &&
            !watch_wifi_change_failed_ && !watch_voice_save_failed_;
#if CONFIG_AUDREAM_WATCH_ANCS
        watch_boot_ready = watch_boot_ready && !watch_notifications_save_failed_;
#endif
        const auto* identity = firmware_release::CurrentIdentity();
        const bool assets_compatible = identity && Assets::GetInstance().SupportsFirmware(identity->sequence);
        watch_boot_ready = watch_boot_ready && assets_compatible;
        if (!assets_compatible) {
            ESP_LOGE(TAG, "Watch boot confirmation withheld because model assets are not confirmed for this firmware");
        }
        watch_boot_health_.Begin(watch_boot_ready, esp_timer_get_time() / 1000);
        if (!watch_boot_ready) ESP_LOGE(TAG, "Watch boot confirmation withheld after initialization failure");
    }
}

void Application::Run() {
    main_task_handle_ = xTaskGetCurrentTaskHandle();
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_TOGGLE_CAMERA |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_PROTOCOL_RECEIVE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        // Only a pending speech handoff needs fast owner polling. Ordinary
        // watch standby still sleeps until an event or the existing clock tick.
        const auto wait_ticks = assistant_capture_prefix_.empty() ? portMAX_DELAY : pdMS_TO_TICKS(20);
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, wait_ticks);

        if (bits & MAIN_EVENT_ERROR) {
            watch_boot_health_.Observe(esp_timer_get_time() / 1000, false);
            PollWatchAssetHealth(false);
            if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
                watch_assistant_.Fail(WatchAssistantError::kProtocol);
                EndWatchAssistantSession();
                RefreshWatchDisplay();
            }
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CAMERA) {
            HandleToggleCameraEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (!HasAssistantSession()) continue;
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (watch_speech_interrupt_) PollWatchSpeechInterrupt();
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if ((bits & MAIN_EVENT_PROTOCOL_RECEIVE) && protocol_) {
            protocol_->Poll();
        }

        if (!assistant_capture_prefix_.empty()) PollWatchAssistantSession();

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            if (protocol_ && (protocol_->HasPendingClose() ||
                UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile()))) {
                // A missing turn acknowledgement must expire even without network input.
                protocol_->Poll();
            }
            if (Board::GetInstance().GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
                PollWatchAssistantSession();
#if CONFIG_AUDREAM_WATCH_UI
                PollWatchFirmware();
#endif
                PollWatchNetwork();
                PollWatchBootstrap();
                PollWatchWakeListening();
                LocalRecordingService::GetInstance().RecoverFailedStart(audio_service_);
                PollWatchStationRecovery();
                const auto events = GetWatchTimeService().Poll(ReadWatchClock());
                if (events.timer_expired || !events.alarms.empty()) {
                    AudreamPowerManager::GetInstance().NotifyUserActivity("watch_time_alert");
                    ESP_LOGI(TAG, "Watch time alert: timer=%d alarms=%u", events.timer_expired,
                        static_cast<unsigned>(events.alarms.size()));
                }
#if CONFIG_AUDREAM_WATCH_UI
                if (watch_alert_sound_) {
                    const auto time = GetWatchTimeService().Snapshot(ReadWatchClock());
                    const auto alerts = static_cast<uint8_t>(time.state.alarm_alerts |
                        (time.state.checkpoint.timer_alert ? 0x20 : 0));
                    watch_alert_sound_->Poll(alerts, static_cast<uint64_t>(esp_timer_get_time() / 1000),
                        events.timer_expired || !events.alarms.empty());
                }
#endif
                RefreshWatchDisplay();
            }
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            if (Board::GetInstance().GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder &&
                watch_boot_health_.Observe(esp_timer_get_time() / 1000, !(bits & MAIN_EVENT_ERROR))) {
                auto& power = AudreamPowerManager::GetInstance();
                bool confirmed = false;
                if (power.BeginStorageRead()) {
                    confirmed = Ota::MarkCurrentVersionValid();
                    power.EndStorageRead();
                }
                watch_boot_health_.Complete(confirmed);
                if (!confirmed) ESP_LOGE(TAG, "Watch boot confirmation failed; no success is assumed");
            }
            PollWatchAssetHealth(!(bits & MAIN_EVENT_ERROR));
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::RefreshWatchDisplay() {
#if CONFIG_AUDREAM_WATCH_UI && CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING && defined(CONFIG_BT_BLUEDROID_ENABLED)
    Blufi::GetInstance().PollLinkPower();
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    WatchPairingGattServer::Get().Poll();
#endif
#if CONFIG_AUDREAM_WATCH_ANCS
    WatchAncsClient::Get().Poll();
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
    auto& android_notifications = WatchAndroidGattServer::Get();
    android_notifications.Poll();
    if (android_notifications.HasPendingOwnerProof() && AudreamPowerManager::GetInstance().BeginStorageRead()) {
        android_notifications.ProcessOwnerProof();
        AudreamPowerManager::GetInstance().EndStorageRead();
    }
#endif
#endif
    auto& board = Board::GetInstance();
    if (board.GetRuntimeProfile() != BoardRuntimeProfile::kWatchAssistantRecorder) { return; }
    WatchViewState state;
#if CONFIG_AUDREAM_WATCH_UI
    state.firmware = BuildWatchFirmwareSnapshot();
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    state.pairing = PollWatchPairing();
#endif
#if CONFIG_AUDREAM_WATCH_ANCS
    state.notifications = BuildWatchNotificationSnapshot(WatchAncsClient::Get().Snapshot()
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
        , WatchAndroidGattServer::Get().Snapshot()
#endif
    );
    state.notifications.save_failed = watch_notifications_save_failed_;
#endif
    state.clock = ReadWatchClock();
    state.time = GetWatchTimeService().Snapshot(state.clock);
    bool discharging = false;
    if (!board.GetBatteryLevel(state.battery_percent, state.charging, discharging)) { state.battery_percent = -1; }
    state.wifi_connected = WifiManager::GetInstance().IsConnected();
    state.ble_connected = Blufi::GetInstance().IsConnected();
    state.recorder = BuildWatchRecordingSnapshot();
    state.recording = state.recorder.active;
    state.recording_ms = state.recorder.duration_ms;
    state.assistant = BuildWatchAssistantSnapshot();
    state.library = watch_library_.Snapshot();
    state.deletion = watch_deletion_;
    state.can_delete_recording = audio_session_coordinator_.Mode() == AudioSessionMode::kIdle &&
        !GetWatchWifiGate().TransferActive() && !state.recorder.active && !state.recorder.control.starting &&
        state.recorder.storage_mounted && audio_service_.IsIdle() && !watch_library_request_;
#if CONFIG_AUDREAM_WATCH_UI
    if (watch_alert_sound_) state.alert_sound = watch_alert_sound_->Snapshot();
    if (watch_playback_) {
        state.playback = watch_playback_->Snapshot();
        state.can_play_recording = state.can_delete_recording && !state.playback.resources_held;
    }
#endif
    state.motion = board.GetWatchMotionSnapshot();
    state.wrist = board.GetWatchWristSnapshot();
    state.controls.brightness = board.GetBacklight() ? ReadWatchLevel(WatchLevel::kBrightness) : -1;
    state.controls.volume = ReadWatchLevel(WatchLevel::kVolume);
    state.controls.can_adjust = !GetWatchWifiGate().TransferActive() && audio_session_coordinator_.Mode() == AudioSessionMode::kIdle &&
        !state.recorder.active && !state.recorder.control.starting && audio_service_.IsIdle();
    state.controls.voice_enabled = audio_service_.IsAssistantUserEnabled();
    state.controls.voice_save_failed = watch_voice_save_failed_;
    state.controls.wifi_enabled = GetWatchWifiGate().Enabled();
    state.controls.wifi_change_failed = watch_wifi_change_failed_;
    state.power_mode = BuildWatchPowerModeSnapshot();
    board.GetDisplay()->UpdateWatchState(state);
}

WatchRecordingSnapshot Application::BuildWatchRecordingSnapshot() {
    WatchRecordingSnapshot snapshot;
    snapshot.control = Board::GetInstance().GetWatchRecordingControlStatus();
    const auto current = LocalRecordingService::GetInstance().GetSnapshot();
    using State = LocalRecordingService::State;
    snapshot.active = current.state != State::kClosed;
    snapshot.paused = current.state == State::kPaused;
    snapshot.stopping = current.state == State::kStopping;
    snapshot.control.starting |= current.state == State::kOpening;
    snapshot.transitioning = current.state == State::kOpening || current.state == State::kPausing ||
        current.state == State::kResuming || snapshot.stopping;
    snapshot.duration_ms = snapshot.active ? current.duration_ms : current.last.valid ? current.last.duration_ms : 0;
    snapshot.last_saved = current.last.valid && snapshot.control.error != WatchRecordingError::kSaveFailed;
    snapshot.last_file = current.last.file_name;
    snapshot.storage_mounted = SdCardStorage::GetInstance().GetStatus().mounted;
    const auto mode = audio_session_coordinator_.Mode();
    snapshot.resources_blocked = mode != AudioSessionMode::kIdle &&
        mode != AudioSessionMode::kLocalRecording && mode != AudioSessionMode::kRecordingPaused &&
        !AudioSessionCoordinator::IsAssistant(mode) && !AudioSessionCoordinator::IsLocalPlayback(mode);
    return snapshot;
}

WatchAssistantSnapshot Application::BuildWatchAssistantSnapshot() {
    const bool cloud_ready = watch_cloud_authorization_ && watch_cloud_authorization_->Usable(esp_timer_get_time() / 1000);
    const bool configured = watch_cloud_required_ ? cloud_ready :
        (protocol_ || !Settings("websocket", false).GetString("url").empty());
    auto mode = audio_session_coordinator_.Mode();
    if (mode == AudioSessionMode::kAssistantListening && !audio_service_.IsAudioProcessorRunning()) mode = AudioSessionMode::kAssistantConnecting;
    auto snapshot = watch_assistant_.Snapshot(mode,
        assistant_session_.IsClosing() || reset_protocol_pending_, pending_assistant_listening_.has_value(),
        configured, GetWatchWifiGate().Enabled() && GetWatchWifiGate().StationAllowed() && WifiManager::GetInstance().IsConnected(), assistant_response_finished_ && !audio_service_.IsIdle(),
        audio_service_.IsAssistantUserEnabled(), assistant_conversation_idle_deadline_us_ != 0);
    if (!watch_capture_guard_ready_) snapshot.can_talk = snapshot.can_interrupt = false;
    snapshot.can_connect = CanPrepareWatchNetwork();
    if (!GetWatchWifiGate().StationAllowed()) snapshot.can_talk = snapshot.can_interrupt = false;
    if (watch_network_failed_ && snapshot.can_connect) {
        snapshot.phase = WatchAssistantPhase::kFailed;
        snapshot.error = WatchAssistantError::kNetwork;
    }
    if (watch_network_preparing_) {
        snapshot.phase = WatchAssistantPhase::kConnecting;
        snapshot.can_talk = snapshot.can_interrupt = snapshot.can_connect = false;
        snapshot.can_end = true;
    }
    if (watch_cloud_task_.Active() && audio_service_.IsAssistantUserEnabled() &&
        GetWatchWifiGate().Enabled() && mode == AudioSessionMode::kIdle) {
        snapshot.phase = WatchAssistantPhase::kConnecting;
        snapshot.can_talk = snapshot.can_interrupt = false;
    }
    return snapshot;
}

void Application::CancelWatchLibrary() {
    if (!watch_library_request_) return;
    watch_library_.Cancel();
    const auto request = watch_library_request_;
    watch_library_request_ = 0;
    AudreamPowerManager::GetInstance().EndStorageRead();
    Board::GetInstance().GetDisplay()->CompleteWatchRequest(request, false);
}

void Application::PollWatchLibrary(uint64_t request_id) {
    if (!watch_library_request_ || request_id != watch_library_request_) return;
    const auto mode = audio_session_coordinator_.Mode();
    if (mode != AudioSessionMode::kIdle || esp_timer_get_time() >= watch_library_deadline_us_) {
        CancelWatchLibrary();
        RefreshWatchDisplay();
        return;
    }
    if (watch_library_.Step()) {
        Schedule([this, request_id]() { PollWatchLibrary(request_id); });
        return;
    }
    const auto request = watch_library_request_;
    watch_library_request_ = 0;
    AudreamPowerManager::GetInstance().EndStorageRead();
    RefreshWatchDisplay();
    Board::GetInstance().GetDisplay()->CompleteWatchRequest(request,
        watch_library_.Snapshot().status == WatchLibraryStatus::kReady);
}

void Application::HandleWatchViewRequest(const WatchViewRequest& request) {
    auto& board = Board::GetInstance();
    if (board.GetRuntimeProfile() != BoardRuntimeProfile::kWatchAssistantRecorder || request.id == 0) { return; }
    AudreamPowerManager::GetInstance().NotifyUserActivity("watch_control");
    auto& service = GetWatchTimeService();
    const auto clock = ReadWatchClock();
    bool succeeded = false;
    switch (request.action) {
        case WatchViewAction::kCheckFirmware:
        case WatchViewAction::kInstallFirmware:
        case WatchViewAction::kCancelFirmware:
        case WatchViewAction::kRebootFirmware:
        case WatchViewAction::kSelectUpdateTarget:
        case WatchViewAction::kValidateModels:
        case WatchViewAction::kCancelModelValidation:
        case WatchViewAction::kRollbackModels:
#if CONFIG_AUDREAM_WATCH_UI
            succeeded = HandleWatchFirmwareRequest(request);
            // Publish the cancellable runtime state before clearing the queued
            // UI request, so navigating away cannot lose cancellation ownership.
            RefreshWatchDisplay();
            board.GetDisplay()->CompleteWatchRequest(request.id, succeeded);
            return;
#endif
            break;
        case WatchViewAction::kOpenDevicePairing:
        case WatchViewAction::kCancelDevicePairing:
        case WatchViewAction::kConfirmDevicePairingPeer:
        case WatchViewAction::kApproveDevicePairingAccount:
        case WatchViewAction::kOpenBindingRemoval:
        case WatchViewAction::kConfirmBindingRemoval:
        case WatchViewAction::kOpenControlRemoval:
        case WatchViewAction::kConfirmControlRemoval:
        case WatchViewAction::kOpenSavedControlPeers:
        case WatchViewAction::kSelectSavedControlPeer:
        case WatchViewAction::kSelectAllSavedControlPeers:
        case WatchViewAction::kConfirmAllControlRemoval:
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
            succeeded = HandleWatchPairingRequest(request);
#endif
            break;
        case WatchViewAction::kSetNotificationsEnabled: {
#if CONFIG_AUDREAM_WATCH_ANCS
            {
                std::lock_guard<std::mutex> lock(watch_notifications_mutex_);
                if (watch_notifications_request_ != request.id) break;
            }
            auto& client = WatchAncsClient::Get();
            // Stop collection before a potentially failing storage operation.
            if (!request.enabled) client.SetEnabled(false);
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
            if (!request.enabled) WatchAndroidGattServer::Get().SetEnabled(false);
#endif
            if (AudreamPowerManager::GetInstance().BeginStorageRead()) {
                succeeded = SaveWatchNotificationsEnabled(request.enabled);
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
                if (succeeded && request.enabled) {
                    watch_voice::WithBoundIdentity([](std::string_view, uint64_t) { return true; });
                }
#endif
                AudreamPowerManager::GetInstance().EndStorageRead();
            }
            watch_notifications_save_failed_ = !succeeded;
            if (request.enabled && succeeded) {
                std::lock_guard<std::mutex> lock(watch_notifications_mutex_);
                succeeded = watch_notifications_request_ == request.id && client.SetEnabled(true);
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
                if (succeeded) WatchAndroidGattServer::Get().SetEnabled(true);
#endif
            }
#endif
            break;
        }
        case WatchViewAction::kConfirmNotificationPeer:
#if CONFIG_AUDREAM_WATCH_ANCS
            succeeded = WatchAncsClient::Get().Confirm(request.notification_confirmation, request.enabled);
#endif
            break;
        case WatchViewAction::kOpenNotification:
#if CONFIG_AUDREAM_WATCH_ANCS
            if (request.notification_source == WatchNotificationSource::kAncs) {
                succeeded = WatchAncsClient::Get().MarkRead(request.notification_session,
                    request.notification_uid, request.notification_revision);
                if (succeeded) WatchAncsClient::Get().RequestAppName(request.notification_session,
                    request.notification_uid, request.notification_revision);
            }
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
            else if (request.notification_source == WatchNotificationSource::kAndroid) {
                succeeded = WatchAndroidGattServer::Get().MarkRead(request.notification_session,
                    request.notification_uid, request.notification_revision);
            }
#endif
#endif
            break;
        case WatchViewAction::kClearNotifications:
#if CONFIG_AUDREAM_WATCH_ANCS
            if (request.notification_source == WatchNotificationSource::kAncs)
                succeeded = WatchAncsClient::Get().Clear(request.notification_session);
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
            else if (request.notification_source == WatchNotificationSource::kAndroid)
                succeeded = WatchAndroidGattServer::Get().Clear(request.notification_session);
#endif
#endif
            break;
        case WatchViewAction::kWristCommand:
            if (board.RequestWatchWrist(request.id, request.wrist_action, request.enabled)) {
                RefreshWatchDisplay();
                return;
            }
            break;
        case WatchViewAction::kResetCorruptActivityStorage:
            if (board.ResetCorruptWatchActivityStorage(request.id, request.repair_token)) {
                RefreshWatchDisplay();
                return;
            }
            break;
        case WatchViewAction::kRetryActivityStorage:
            if (board.RetryWatchActivityStorage(request.id)) {
                // The worker snapshot completes this request after actual storage I/O.
                RefreshWatchDisplay();
                return;
            }
            break;
        case WatchViewAction::kTimeCommand:
#if CONFIG_AUDREAM_WATCH_UI
            if (request.command == WatchTimeCommand::kDismissAlerts && watch_alert_sound_) watch_alert_sound_->Silence();
#endif
            succeeded = service.Execute(request.command, clock, request.duration_ms);
            break;
        case WatchViewAction::kSetAlarm:
            succeeded = service.SetAlarm(request.alarm_index, request.alarm, clock);
            break;
        case WatchViewAction::kSetPreferences:
            succeeded = service.SetPreferences(request.timezone, request.hour24, clock);
            break;
        case WatchViewAction::kSeekPlayback:
#if CONFIG_AUDREAM_WATCH_UI
            if (watch_playback_ && watch_playback_->Seek(request.playback_generation, request.id, request.playback_target_sample)) {
                RefreshWatchDisplay();
                return;
            }
#endif
            break;
        case WatchViewAction::kSetPlaybackVolume:
#if CONFIG_AUDREAM_WATCH_UI
            if (watch_playback_ && watch_playback_->SetVolume(request.playback_generation, request.id, request.level_value)) {
                // The view acknowledges only the worker's completed transaction.
                RefreshWatchDisplay();
                return;
            }
#endif
            break;
        case WatchViewAction::kSetLevel: {
            if (!IsValidWatchLevel(request.level, request.level_value) ||
                audio_session_coordinator_.Mode() != AudioSessionMode::kIdle ||
                !audio_service_.IsIdle() || board.GetWatchRecordingControlStatus().starting ||
                !AudreamPowerManager::GetInstance().BeginStorageRead()) break;
            const int previous = ReadWatchLevel(request.level);
            if (previous >= 0 && request.level == WatchLevel::kBrightness && board.GetBacklight()) {
                succeeded = SaveWatchLevel(request.level, request.level_value);
                if (succeeded) board.GetBacklight()->RestoreBrightness();
            } else if (previous >= 0 && request.level == WatchLevel::kVolume) {
                auto* codec = board.GetAudioCodec();
                if (codec && codec->SetWatchOutputVolume(request.level_value)) {
                    succeeded = SaveWatchLevel(request.level, request.level_value);
                    if (!succeeded && !codec->SetWatchOutputVolume(previous)) {
                        ESP_LOGE(TAG, "Failed to restore speaker volume after a watch settings save failure");
                    }
                }
            }
            AudreamPowerManager::GetInstance().EndStorageRead();
            break;
        }
        case WatchViewAction::kSetWifiEnabled: {
            auto& gate = GetWatchWifiGate();
            if (!gate.IsCurrent(request.id)) break;
#if CONFIG_AUDREAM_WATCH_UI
            if (!StopWatchFirmware(0)) {
                if (!watch_firmware_ || !watch_firmware_->Read().active) break;
                if (watch_deferred_wifi_request_)
                    board.GetDisplay()->CompleteWatchRequest(watch_deferred_wifi_request_->id, false);
                watch_deferred_wifi_request_ = std::make_unique<WatchViewRequest>(request);
                return;
            }
#endif
            CancelWatchNetwork();
            watch_cloud_task_.Cancel();
            watch_cloud_authorization_.reset();
            watch_cloud_retry_after_us_ = 0;
            if (watch_cloud_task_.Active()
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
                || (!watch_pairing_control_only_.load() && !watch_pairing_removal_.load() &&
                    !watch_pairing_control_removal_.load() && !watch_pairing_saved_removal_.load() && !StopWatchPairing(0))
#endif
            ) {
                if (watch_deferred_wifi_request_) {
                    board.GetDisplay()->CompleteWatchRequest(watch_deferred_wifi_request_->id, false);
                }
                watch_deferred_wifi_request_ = std::make_unique<WatchViewRequest>(request);
                return;
            }
            watch_station_recovery_.Cancel();
            watch_wifi_change_failed_ = true;
            if (!request.enabled && (assistant_session_.Token() || pending_assistant_listening_)) EndWatchAssistantSession();
            if (gate.TransferActive() || !audio_service_.IsIdle() || board.GetWatchRecordingControlStatus().starting ||
                reset_protocol_pending_ || assistant_session_.IsClosing()) break;
            // A reservation closes the idle-check race with recording, AP
            // transfer and sleep. No radio operation runs in an LVGL callback.
            const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance);
            if (!reservation) break;
            if (AudreamPowerManager::GetInstance().BeginStorageRead()) {
                if (request.enabled) {
                    succeeded = SaveWatchWifiEnabled(true) && gate.ConfirmEnabled(request.id) &&
                        board.ApplyWatchWifiEnabled(true);
                } else {
                    const bool stopped = board.ApplyWatchWifiEnabled(false);
                    const bool saved = SaveWatchWifiEnabled(false);
                    succeeded = stopped && saved;
                }
                AudreamPowerManager::GetInstance().EndStorageRead();
            }
            audio_session_coordinator_.Fail(reservation, true);
            watch_wifi_change_failed_ = !succeeded;
            if (succeeded && request.enabled) watch_station_policy_.Request(esp_timer_get_time() / 1000);
            break;
        }
        case WatchViewAction::kSetPowerMode:
            succeeded = RequestWatchPowerMode(request.settings_revision, request.binding_generation, request.power_mode);
            break;
        case WatchViewAction::kPrepareAssistantNetwork:
            succeeded = RequestWatchNetwork();
            break;
        case WatchViewAction::kSetVoiceEnabled: {
            {
                std::lock_guard<std::mutex> lock(watch_voice_mutex_);
                if (watch_voice_request_ != request.id) break;
            }
            watch_cloud_task_.Cancel();
            watch_cloud_authorization_.reset();
            watch_cloud_retry_after_us_ = 0;
            if (!request.enabled) {
                audio_service_.SetAssistantUserEnabled(false);
                watch_press_.CancelPending();
                if (assistant_session_.Token() || pending_assistant_listening_) EndWatchAssistantSession();
                audio_service_.EnableWakeWordDetection(false);
            } else if (audio_session_coordinator_.Mode() != AudioSessionMode::kIdle ||
                !audio_service_.IsIdle() || board.GetWatchRecordingControlStatus().starting ||
                reset_protocol_pending_ || assistant_session_.IsClosing()) {
                break;
            }
            if (AudreamPowerManager::GetInstance().BeginStorageRead()) {
                succeeded = SaveWatchVoiceEnabled(request.enabled);
                AudreamPowerManager::GetInstance().EndStorageRead();
            }
            watch_voice_save_failed_ = !succeeded;
            // Enabling is never optimistic. Failed disable persistence must not
            // turn the microphone back on for this boot.
            if (succeeded && request.enabled) {
                std::lock_guard<std::mutex> lock(watch_voice_mutex_);
                if (watch_voice_request_ == request.id) audio_service_.SetAssistantUserEnabled(true);
            }
            break;
        }
        case WatchViewAction::kAssistantTalk: {
            const auto current = BuildWatchAssistantSnapshot();
            if (current.can_talk || current.can_interrupt) succeeded = RequestWatchListening(kListeningModeManualStop);
            break;
        }
        case WatchViewAction::kAssistantPress: {
            if (!watch_press_.Held(request.press_token, esp_timer_get_time() / 1000)) {
                succeeded = true;
                break;
            }
            const auto current = BuildWatchAssistantSnapshot();
            if (current.can_talk || current.can_interrupt) succeeded = RequestWatchListening(kListeningModeManualStop, "", request.press_token);
            break;
        }
        case WatchViewAction::kAssistantRelease:
        case WatchViewAction::kAssistantCancelPress:
            succeeded = FinishWatchPress(request.press_token);
            break;
        case WatchViewAction::kAssistantSend:
            if (BuildWatchAssistantSnapshot().can_send) {
                HandleStopListeningEvent();
                succeeded = HasAssistantSession();
            }
            break;
        case WatchViewAction::kAssistantEnd:
            CancelWatchNetwork();
            EndWatchAssistantSession();
            succeeded = true;
            break;
        case WatchViewAction::kAssistantClear:
            if (!HasAssistantSession() && !assistant_session_.IsClosing() && !pending_assistant_listening_) {
                watch_assistant_.Clear();
                succeeded = true;
            }
            break;
        case WatchViewAction::kRecordingCommand:
            if (BuildWatchRecordingSnapshot().Can(request.recording_command)) {
                succeeded = board.ExecuteWatchRecordingCommand(request.recording_command);
            }
            break;
        case WatchViewAction::kLoadRecordings:
        case WatchViewAction::kLoadOlderRecordings:
        case WatchViewAction::kLoadNewerRecordings:
            if (request.action != WatchViewAction::kLoadRecordings &&
                (request.library_revision != watch_library_.Snapshot().revision ||
                 watch_library_.Snapshot().status != WatchLibraryStatus::kReady)) break;
            if ((request.action == WatchViewAction::kLoadOlderRecordings && !watch_library_.Snapshot().has_older) ||
                (request.action == WatchViewAction::kLoadNewerRecordings && !watch_library_.Snapshot().has_newer)) break;
            CancelWatchLibrary();
            if (audio_session_coordinator_.Mode() != AudioSessionMode::kIdle) {
                watch_library_.Cancel();
                break;
            }
            if (!AudreamPowerManager::GetInstance().BeginStorageRead()) {
                watch_library_.Cancel();
                break;
            }
            if (!SdCardStorage::GetInstance().GetStatus().mounted ||
                !(request.action == WatchViewAction::kLoadOlderRecordings ? watch_library_.BeginOlder() :
                  request.action == WatchViewAction::kLoadNewerRecordings ? watch_library_.BeginNewer() :
                  watch_library_.Begin(SdCardStorage::GetInstance().RecordingsDirectory()))) {
                watch_library_.Cancel(WatchLibraryStatus::kUnavailable);
                AudreamPowerManager::GetInstance().EndStorageRead();
                break;
            }
            watch_library_request_ = request.id;
            watch_library_deadline_us_ = esp_timer_get_time() + 15000000;
            Schedule([this, request_id = request.id]() { PollWatchLibrary(request_id); });
            RefreshWatchDisplay();
            return;
        case WatchViewAction::kDeleteRecording: {
            const bool retry = request.metadata_only && request.cleanup_request && request.cleanup_request == watch_deletion_.request &&
                request.recording_id == watch_deletion_.recording_id && watch_deletion_.cleanup_available;
            const auto& entries = watch_library_.Snapshot().entries;
            const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& item) {
                return item.id == request.recording_id;
            });
            const uint64_t expected_bytes = entry == entries.end() ? 0 : entry->size_bytes;
            const bool current = !request.cleanup_request && request.library_revision == watch_library_.Snapshot().revision &&
                watch_library_.Snapshot().status == WatchLibraryStatus::kReady && entry != entries.end() &&
                request.metadata_only == !entry->audio_available;
            const bool cleanup = retry || (current && request.metadata_only);
            watch_deletion_ = {request.id, request.recording_id, {}, cleanup};
            if ((!current && !retry) || watch_library_request_ || !audio_service_.IsIdle() ||
                GetWatchWifiGate().TransferActive() || board.GetWatchRecordingControlStatus().starting ||
                LocalRecordingService::GetInstance().GetSnapshot().state != LocalRecordingService::State::kClosed) break;
            const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance);
            if (!reservation) break;
            if (AudreamPowerManager::GetInstance().BeginStorageRead()) {
                watch_deletion_.result = SdCardStorage::GetInstance().DeleteRecordingChecked(
                    request.recording_id, expected_bytes, cleanup);
                watch_deletion_.cleanup_available = watch_deletion_.result.audio_absent &&
                    watch_deletion_.result.status != RecordingDeleteStatus::kDeleted;
                if (watch_deletion_.result.audio_absent) {
                    watch_library_.Forget(request.recording_id);
                    LocalRecordingService::GetInstance().ForgetDeletedRecording(request.recording_id);
                }
                succeeded = watch_deletion_.result.status == RecordingDeleteStatus::kDeleted;
                AudreamPowerManager::GetInstance().EndStorageRead();
            }
            audio_session_coordinator_.Fail(reservation, true);
            break;
        }
        case WatchViewAction::kPlayRecording:
        case WatchViewAction::kPausePlayback:
        case WatchViewAction::kResumePlayback:
        case WatchViewAction::kStopPlayback: {
#if CONFIG_AUDREAM_WATCH_UI
            if (!watch_playback_) break;
            if (request.action != WatchViewAction::kPlayRecording) {
                succeeded = request.action == WatchViewAction::kStopPlayback
                    ? watch_playback_->RequestStop(request.playback_generation)
                    : watch_playback_->SetPaused(request.playback_generation, request.action == WatchViewAction::kPausePlayback);
                break;
            }
            const auto& library = watch_library_.Snapshot();
            if (library.status != WatchLibraryStatus::kReady || request.library_revision != library.revision ||
                watch_library_request_ || !audio_service_.IsIdle() || GetWatchWifiGate().TransferActive() ||
                board.GetWatchRecordingControlStatus().starting ||
                LocalRecordingService::GetInstance().GetSnapshot().state != LocalRecordingService::State::kClosed) break;
            const auto entry = std::find_if(library.entries.begin(), library.entries.end(), [&](const auto& item) {
                return item.id == request.recording_id;
            });
            // Catalog entries have validated IDs and filenames. The worker owns
            // the storage lease before opening and checking the actual file size.
            if (entry == library.entries.end() || !entry->audio_available || !entry->size_bytes) break;
            succeeded = watch_playback_->Start(entry->id,
                SdCardStorage::GetInstance().RecordingsDirectory() + "/" + entry->file_name, entry->size_bytes) != 0;
#endif
            break;
        }
        case WatchViewAction::kCancelRecordings:
            CancelWatchLibrary();
            succeeded = true;
            break;
    }
    RefreshWatchDisplay();
    board.GetDisplay()->CompleteWatchRequest(request.id, succeeded);
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
        // Cached assistant credentials can be used without running legacy OTA,
        // asset downloads, or activation prompts over the offline watch face.
        watch_cloud_retry_after_us_ = 0;
        if (!protocol_) InitializeProtocol();
        RefreshWatchDisplay();
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
        watch_cloud_task_.Cancel();
        watch_cloud_authorization_.reset();
        if (HasAssistantSession()) watch_assistant_.Fail(WatchAssistantError::kNetwork);
        EndWatchAssistantSession();
        RefreshWatchDisplay();
        return;
    }
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (protocol_ && (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking)) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    InitializeProtocol();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url;
    if (board.GetRuntimeProfile() != BoardRuntimeProfile::kWatchAssistantRecorder) {
        download_url = settings.GetString("download_url");
    }

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Applying the selected trial is part of its local health gate. A broken
    // font, image, or index must remain unconfirmed and recover on restart.
    if (!assets.Apply()) {
        assets.EndTrialHealth(true);
        ESP_LOGE(TAG, "Selected assets failed local display initialization");
    }
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    if (UsesOfflineWatchStartup(board.GetRuntimeProfile()) &&
        (!GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed())) return;
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (UsesOfflineWatchStartup(board.GetRuntimeProfile())) {
        auto& power = AudreamPowerManager::GetInstance();
        if (!power.BeginStorageRead()) return;
        watch_voice::Binding binding;
        const auto binding_status = watch_voice::LoadBinding(binding);
        power.EndStorageRead();
        if (binding_status != watch_voice::BindingStatus::kAbsent) watch_cloud_required_ = true;
        if (watch_cloud_required_) {
            if (binding_status != watch_voice::BindingStatus::kBound) return;
            if (!watch_cloud_authorization_ || binding.device_id != watch_cloud_device_id_ ||
                binding.generation != watch_cloud_binding_generation_ ||
                !watch_cloud_authorization_->Usable(esp_timer_get_time() / 1000)) {
                StartWatchBootstrap(binding);
                return;
            }
            if (!audio_service_.IsAssistantUserEnabled()) return;
            auto websocket = std::make_unique<WebsocketProtocol>();
            if (!websocket->SetCloudAuthorization(*watch_cloud_authorization_, binding.device_id)) return;
            protocol_ = std::move(websocket);
        } else {
            Settings settings("websocket", false);
            if (settings.GetString("url").empty()) {
                ESP_LOGI(TAG, "Assistant setup is required; offline watch and recording remain available");
                return;
            }
            protocol_ = std::make_unique<WebsocketProtocol>();
        }
    } else if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnReceiveReady([this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PROTOCOL_RECEIVE);
    });

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
            const auto token = assistant_session_.Token();
            Schedule([this, token]() {
                if (!assistant_session_.IsActive(token)) return;
                watch_assistant_.Fail(WatchAssistantError::kNetwork);
                EndWatchAssistantSession();
                RefreshWatchDisplay();
            });
            return;
        }
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (!HasAssistantSession() || !AllowsAssistantSession()) return;
        if (GetDeviceState() == kDeviceStateSpeaking) {
            const bool accepted = audio_service_.PushPacketToDecodeQueue(std::move(packet));
            if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
                assistant_response_deadline_us_ = esp_timer_get_time() + 30000000LL;
                if (!accepted) {
                    ESP_LOGW(TAG, "Assistant playback queue rejected audio; closing the session");
                    watch_assistant_.Fail(WatchAssistantError::kAudio);
                    EndWatchAssistantSession();
                }
            }
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (UsesOfflineWatchStartup(board.GetRuntimeProfile())) {
            const auto token = assistant_session_.Token();
            Schedule([this, token]() {
                if (assistant_session_.IsActive(token)) {
                    if (!assistant_response_finished_) watch_assistant_.Fail(WatchAssistantError::kNetwork);
                    EndWatchAssistantSession();
                    RefreshWatchDisplay();
                }
            });
            return;
        }
        ScheduleProtocol([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        if (!HasAssistantSession()) return;
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) return;
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) return;
            if (strcmp(state->valuestring, "start") == 0) {
                ScheduleProtocol([this]() {
                    aborted_ = false;
                    assistant_response_finished_ = false;
                    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
                        assistant_stop_listening_pending_ = false;
                        assistant_response_deadline_us_ = esp_timer_get_time() + 30000000LL;
                    }
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ScheduleProtocol([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
                            assistant_response_finished_ = true;
                            assistant_response_deadline_us_ = esp_timer_get_time() + 30000000LL;
                            audio_service_.EnableVoiceProcessing(false);
                            assistant_session_.Update(AudioSessionMode::kAssistantThinking, assistant_session_.Token());
                            SetDeviceState(kDeviceStateIdle);
                            return;
                        }
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    ScheduleProtocol([this, display, message = std::string(text->valuestring)]() {
                        if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
                            watch_assistant_.AppendAssistantText(message);
                            RefreshWatchDisplay();
                        } else {
                            display->SetChatMessage("assistant", message.c_str());
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                ScheduleProtocol([this, display, message = std::string(text->valuestring)]() {
                    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile()) &&
                        GetDeviceState() == kDeviceStateListening) {
                        audio_service_.EnableVoiceProcessing(false);
                        assistant_session_.Update(AudioSessionMode::kAssistantThinking, assistant_session_.Token());
                        assistant_response_deadline_us_ = esp_timer_get_time() + 120000000LL;
                        SetDeviceState(kDeviceStateIdle);
                    }
                    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
                        watch_assistant_.SetUserText(message);
                        RefreshWatchDisplay();
                    } else {
                        display->SetChatMessage("user", message.c_str());
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                ScheduleProtocol([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    ScheduleProtocol([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                ScheduleProtocol([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
        if (GetDeviceState() == kDeviceStateConnecting || GetDeviceState() == kDeviceStateListening ||
            assistant_session_.IsClosing()) EndWatchAssistantSession();
        else RequestWatchListening(GetDefaultListeningMode());
        return;
    }
    if (!AllowsAssistantSession()) return;
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        ESP_LOGI(TAG, "ToggleChat: stopping listening and closing text result");
        protocol_->CloseAudioChannel();

        // Close text results and restore camera preview.
        CloseTextResultAndRestorePreview();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    if (!AllowsAssistantSession()) return;
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
                watch_assistant_.Fail(WatchAssistantError::kNetwork);
                EndWatchAssistantSession();
                RefreshWatchDisplay();
            }
            return;
        }
    }

    const auto press = assistant_press_token_.load();
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile()) && press &&
        !watch_press_.Held(press, esp_timer_get_time() / 1000)) {
        FinishWatchPress(press);
        RefreshWatchDisplay();
        return;
    }
    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
        RequestWatchListening(kListeningModeManualStop);
        return;
    }
    if (!AllowsAssistantSession()) return;
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
        if (!HasAssistantSession()) return;
        if (GetDeviceState() == kDeviceStateConnecting) {
            EndWatchAssistantSession();
        } else if (GetDeviceState() == kDeviceStateListening) {
            if (!audio_service_.EnableVoiceProcessing(false)) {
                watch_assistant_.Fail(WatchAssistantError::kAudio);
                EndWatchAssistantSession();
                return;
            }
            assistant_capture_deadline_us_ = 0;
            assistant_session_.Update(AudioSessionMode::kAssistantThinking, assistant_session_.Token());
            assistant_stop_listening_pending_ = true;
            assistant_response_deadline_us_ = esp_timer_get_time() + 120000000LL;
            SetDeviceState(kDeviceStateIdle);
            PollWatchAssistantSession();
        }
        return;
    }
    auto state = GetDeviceState();

    ESP_LOGI(TAG, "HandleStopListeningEvent: state=%d", state);

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Stopping listening, will close text result");
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);

        // Close text results and restore camera preview.
        ESP_LOGI(TAG, "Calling CloseTextResultAndRestorePreview");
        CloseTextResultAndRestorePreview();
    } else {
        ESP_LOGI(TAG, "Not in listening state (state=%d), ignoring stop event", state);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
        if (AllowsAssistantSession() && audio_service_.IsWakeWordRunning()) {
            RequestWatchListening(GetDefaultListeningMode(), audio_service_.GetLastWakeWord());
        }
        return;
    }
    if (!AllowsAssistantSession()) return;
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(
                UsesDefaultWakeWordDetection(Board::GetInstance().GetRuntimeProfile()));
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    if (!AllowsAssistantSession()) return;
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
                watch_assistant_.Fail(WatchAssistantError::kNetwork);
                EndWatchAssistantSession();
                return;
            }
            audio_service_.EnableWakeWordDetection(
                UsesDefaultWakeWordDetection(Board::GetInstance().GetRuntimeProfile()));
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);

    // Set flag to play popup sound after state changes to listening
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile()) &&
        (new_state == kDeviceStateListening || new_state == kDeviceStateSpeaking) && !HasAssistantSession()) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(UsesDefaultWakeWordDetection(board.GetRuntimeProfile()));
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            if (UsesOfflineWatchStartup(board.GetRuntimeProfile()) && assistant_press_token_ &&
                !watch_press_.Held(assistant_press_token_, esp_timer_get_time() / 1000)) {
                FinishWatchPress(assistant_press_token_);
                break;
            }
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                if (assistant_wake_turn_ && !assistant_wake_capture_generation_) {
                    const auto generation = assistant_session_.Token();
                    const auto now_ms = esp_timer_get_time() / 1000;
                    if (!watch_voice_capture_.Begin(generation, now_ms) ||
                        (!assistant_capture_prefix_.empty() &&
                         !watch_voice_capture_.SeedSpeech(generation, 240, now_ms))) {
                        EndWatchAssistantSession();
                        break;
                    }
                    assistant_wake_capture_generation_ = generation;
                }
                const bool has_prefix = !assistant_capture_prefix_.empty();
                const auto prefix_samples = assistant_capture_prefix_.size();
                const bool capture_started = has_prefix
                    ? esp_timer_get_time() < pending_assistant_deadline_us_ &&
                      audio_service_.StartCaptureWithPreroll(assistant_session_.Token(), std::move(assistant_capture_prefix_))
                    : audio_service_.EnableVoiceProcessing(true);
                if (!capture_started) {
                    ESP_LOGE(TAG, "Assistant capture could not start");
                    if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
                        watch_assistant_.Fail(WatchAssistantError::kAudio);
                        EndWatchAssistantSession();
                    } else {
                        protocol_->CloseAudioChannel();
                        SetDeviceState(kDeviceStateIdle);
                    }
                    break;
                }
                if (has_prefix) ESP_LOGI(TAG, "Assistant interrupted capture started: generation=%llu prefix_samples=%u",
                    static_cast<unsigned long long>(assistant_session_.Token()), static_cast<unsigned>(prefix_samples));
                if (UsesOfflineWatchStartup(board.GetRuntimeProfile())) {
                    const auto press = assistant_press_token_.load();
                    if (press && !watch_press_.CaptureReady(press, esp_timer_get_time() / 1000)) {
                        FinishWatchPress(press);
                        break;
                    }
                    assistant_capture_deadline_us_ = esp_timer_get_time() + WatchPushToTalk::kCaptureLimitMs * 1000;
                }
                protocol_->SendStartListening(listening_mode_);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(!assistant_press_token_ && audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
                if (watch_speech_interrupt_) watch_speech_interrupt_->Cancel();
                const bool drained = audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
                audio_service_.ResetDecoder();
                const auto generation = assistant_session_.Token();
                if (drained && watch_capture_guard_ready_ && watch_speech_interrupt_ && aec_mode_ == kAecOnDeviceSide &&
                    protocol_ && protocol_->SupportsConversationTurns() &&
                    watch_speech_interrupt_->Begin(generation, esp_timer_get_time() / 1000) &&
                    !audio_service_.StartSpeechMonitor(generation)) {
                    watch_speech_interrupt_->Cancel();
                    ESP_LOGW(TAG, "Speech interruption unavailable; manual interruption remains enabled");
                }
                break;
            }
            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
    RefreshWatchDisplay();
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::ScheduleProtocol(std::function<void()>&& callback) {
    const bool watch = UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile());
    if (watch && main_task_handle_.load() != xTaskGetCurrentTaskHandle()) {
        ESP_LOGW(TAG, "Protocol callbacks must capture their session on the application task");
        return;
    }
    if (!protocol_) return;
    auto guard = protocol_->CaptureEventGuard();
    const auto token = watch ? assistant_session_.Token() : 0;
    Schedule([this, watch, token, guard = std::move(guard), callback = std::move(callback)]() {
        if (guard() && (!watch || assistant_session_.IsActive(token))) callback();
    });
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    if (!HasAssistantSession()) return;
    assistant_response_deadline_us_ = 0;
    assistant_response_finished_ = false;
    assistant_stop_listening_pending_ = false;
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
#if CONFIG_AUDREAM_WATCH_UI
    if (!StopWatchFirmware(3000)) {
        const auto update = watch_firmware_->Read();
        if (update.active || update.phase == watch_update::Phase::kResourceFault) {
            ESP_LOGE(TAG, "Reboot rejected because firmware update resources have not been released");
            return;
        }
    }
#endif
    if (!audio_service_.Stop()) {
        ESP_LOGE(TAG, "Reboot rejected because audio resources have not been released");
        return;
    }
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    if (LocalRecordingService::GetInstance().IsRecording()) {
        ESP_LOGW(TAG, "Firmware upgrade rejected while a recording is open");
        return false;
    }
    auto& board = Board::GetInstance();
    const bool watch = board.GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder;
    const auto reservation = watch ? audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance)
                                   : AudioSessionTransition{};
    if (watch && !reservation) {
        ESP_LOGW(TAG, "Firmware upgrade rejected because watch resources are busy");
        return false;
    }
    struct MaintenanceRelease {
        AudioSessionCoordinator& coordinator;
        AudioSessionTransition transition;
        bool released = true;
        ~MaintenanceRelease() { if (transition) coordinator.Fail(transition, released); }
    } maintenance{audio_session_coordinator_, reservation};
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (!audio_service_.Stop()) {
        maintenance.released = false;
        ESP_LOGE(TAG, "Firmware upgrade rejected because audio resources have not been released");
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    const auto update_progress = [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    };
    const bool upgrade_success = watch
        ? Ota::UpgradeSigned(upgrade_url, update_progress)
        : Ota::Upgrade(upgrade_url, update_progress);

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        if (!audio_service_.Start()) {
            maintenance.released = false;
            ESP_LOGE(TAG, "Audio restart failed after firmware upgrade failure");
        } else {
            SetDeviceState(kDeviceStateIdle);
        }
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
        Schedule([this, wake_word]() { RequestWatchListening(GetDefaultListeningMode(), wake_word); });
        return;
    }
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile()) && audio_session_coordinator_.BlocksSleep()) {
        return false;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && (protocol_->IsAudioChannelOpened() || protocol_->HasPendingClose())) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    ScheduleProtocol([this, payload = std::move(payload)]() {
        if (protocol_ && HasAssistantSession()) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SendText(const std::string& text) {
    // Always schedule to run in main task for thread safety
    Schedule([this, text = std::move(text)]() {
        if (protocol_ && HasAssistantSession()) {
            protocol_->SendUserText(text);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    watch_cloud_task_.Cancel();
    Schedule([this]() {
        if (UsesOfflineWatchStartup(Board::GetInstance().GetRuntimeProfile())) {
            watch_cloud_authorization_.reset();
            watch_cloud_retry_after_us_ = 0;
            reset_protocol_pending_ = true;
            EndWatchAssistantSession();
            PollWatchAssistantSession();
            return;
        }
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

void Application::ToggleCameraMode() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CAMERA);
}

bool Application::PauseCameraPreview() {
#if CONFIG_IDF_TARGET_ESP32S3
    if (camera_display_ != nullptr && camera_display_->IsRunning()) {
        ESP_LOGI(TAG, "Pausing camera preview for photo capture");
        camera_display_->Stop();
        camera_preview_paused_ = true;
        return true;
    }
#endif
    return false;
}

void Application::ResumeCameraPreview() {
#if CONFIG_IDF_TARGET_ESP32S3
    if (camera_preview_paused_ && camera_display_ != nullptr) {
        ESP_LOGI(TAG, "Resuming camera preview after photo capture");
        if (camera_display_->Start()) {
            ESP_LOGI(TAG, "Camera preview resumed successfully");
        } else {
            ESP_LOGE(TAG, "Failed to resume camera preview");
        }
        camera_preview_paused_ = false;
    }
#endif
}

void Application::CloseTextResultAndRestorePreview() {
    auto display = Board::GetInstance().GetDisplay();
    auto lcd_display = dynamic_cast<LcdDisplay*>(display);

    ESP_LOGI(TAG, "CloseTextResultAndRestorePreview called");
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "camera_display_: %p, IsRunning: %d", camera_display_,
             camera_display_ != nullptr ? camera_display_->IsRunning() : -1);
#endif

    // Close the text result widget.
    if (lcd_display != nullptr) {
        lcd_display->CloseTextResult();
        ESP_LOGI(TAG, "Text result closed");
    }

#if CONFIG_IDF_TARGET_ESP32S3
    // Restore camera preview.
    if (camera_display_ != nullptr) {
        if (!camera_display_->IsRunning()) {
            ESP_LOGI(TAG, "Starting camera preview...");
            if (camera_display_->Start()) {
                ESP_LOGI(TAG, "Camera preview restored successfully");
                camera_preview_paused_ = false;
            } else {
                ESP_LOGE(TAG, "Failed to restore camera preview");
            }
        } else {
            ESP_LOGI(TAG, "Camera preview already running");
        }
    } else {
        ESP_LOGE(TAG, "camera_display_ is nullptr, cannot restore preview");
    }
#endif
}

void Application::HandleToggleCameraEvent() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

#if CONFIG_IDF_TARGET_ESP32S3
    // Check whether the board has a camera.
    if (board.GetCamera() == nullptr) {
        ESP_LOGW(TAG, "Board does not have camera");
        display->ShowNotification("该板子不支持相机");
        return;
    }

    // Check whether esp_camera has been initialized.
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        ESP_LOGW(TAG, "Camera not initialized");
        display->ShowNotification("相机未初始化");
        return;
    }

    if (!camera_mode_enabled_) {
        // Start camera mode.
        ESP_LOGI(TAG, "Starting camera mode");

        if (camera_display_ == nullptr) {
            camera_display_ = new CameraDisplay();

            // Use preview_image_ to display the camera preview.
            auto lcd_display = dynamic_cast<LcdDisplay*>(display);
            if (lcd_display != nullptr && lvgl_port_lock(0)) {
                lv_obj_t* preview_img = lcd_display->GetPreviewImage();

                if (preview_img != nullptr) {
                    // preview_image_ is already full-screen by default.
                    camera_display_->SetCameraImage(preview_img);
                    ESP_LOGI(TAG, "Using preview_image for camera display: 320x480");
                }

                lvgl_port_unlock();
            }
        }

        if (camera_display_->Start()) {
            camera_mode_enabled_ = true;
            display->ShowNotification("相机已启动");
        } else {
            ESP_LOGE(TAG, "Failed to start camera mode");
            delete camera_display_;
            camera_display_ = nullptr;
            camera_mode_enabled_ = false;
            display->ShowNotification("相机启动失败");
        }
    } else {
        // Stop camera mode.
        ESP_LOGI(TAG, "Stopping camera mode");

        if (camera_display_ != nullptr) {
            camera_display_->Stop();
            delete camera_display_;
            camera_display_ = nullptr;
        }

        camera_mode_enabled_ = false;
        display->ShowNotification("相机已关闭");
    }
#else
    ESP_LOGW(TAG, "Camera is not supported on this target");
    display->ShowNotification("Camera is not supported");
#endif
}
