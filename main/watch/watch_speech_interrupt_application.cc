#include "application.h"
#include "wifi_station.h"
#include "watch/watch_wifi_gate.h"
#include "esp_timer.h"
#include <algorithm>

bool Application::RequestWatchListening(ListeningMode mode, const std::string& wake_word, uint64_t press_token,
                                       std::vector<int16_t> prefix) {
    const bool interrupted = !prefix.empty();
    if (interrupted && (!watch_capture_guard_ready_ || prefix.size() < WatchSpeechInterrupt::kMinimumSpeechSamples ||
        prefix.size() > WatchSpeechInterrupt::kCapacitySamples || press_token || !wake_word.empty() ||
        GetDeviceState() != kDeviceStateSpeaking || !protocol_ || !protocol_->SupportsConversationTurns())) return false;
    if (watch_network_preparing_) return false;
    if (press_token && (!watch_capture_guard_ready_ || !watch_press_.Held(press_token, esp_timer_get_time() / 1000))) return false;
    if (reset_protocol_pending_ || !AllowsAssistantSession() || wake_word.size() > 128) return false;
    if (!WifiStation::GetInstance().IsConnected()) {
        watch_assistant_.Fail(WatchAssistantError::kNetwork);
        return false;
    }
    if (!GetWatchWifiGate().StationAllowed()) return false;
    if (!protocol_) InitializeProtocol();
    if (!protocol_) return false;
    if (watch_cloud_required_ && (watch_cloud_task_.Active() || !watch_cloud_authorization_ ||
        !watch_cloud_authorization_->Usable(esp_timer_get_time() / 1000))) return false;
    if (!wake_word.empty()) {
        if (!watch_cloud_authorization_ ||
            !watch_cloud_authorization_->settings.SupportsWakeRuntime()) return false;
        // Wake mode uses local VAD delimiting, not the legacy server auto/detect path.
        mode = kListeningModeManualStop;
    }
    if (!press_token && !assistant_press_token_ && HasAssistantSession() && GetDeviceState() == kDeviceStateListening &&
        mode == listening_mode_ && wake_word.empty()) return true;
    const bool reuse = mode == kListeningModeManualStop && wake_word.empty() &&
        HasAssistantSession() && GetDeviceState() != kDeviceStateListening &&
        !assistant_stop_listening_pending_ && protocol_->SuspendIncomingTurn() &&
        assistant_session_.BeginNextTurn(assistant_session_.Token());
    if (interrupted && !reuse) {
        EndWatchAssistantSession();
        return false;
    }
    if (watch_speech_interrupt_) watch_speech_interrupt_->Cancel();
    if (reuse) {
        assistant_wake_capture_generation_ = 0;
        watch_voice_capture_.Cancel();
        watch_press_.Retire(assistant_press_token_.exchange(0));
        assistant_response_finished_ = false;
        assistant_response_deadline_us_ = 0;
        assistant_capture_deadline_us_ = 0;
        assistant_conversation_idle_deadline_us_ = 0;
        SetDeviceState(kDeviceStateIdle);
    } else {
        // Legacy peers cannot safely reuse untagged in-flight audio.
        EndWatchAssistantSession();
    }
    if (press_token && !watch_press_.Held(press_token, esp_timer_get_time() / 1000)) {
        EndWatchAssistantSession();
        return false;
    }
    assistant_press_token_ = press_token;
    // Both wake and speech interruption use the existing local VAD endpoint.
    assistant_wake_turn_ = !wake_word.empty() || interrupted;
    std::fill(assistant_capture_prefix_.begin(), assistant_capture_prefix_.end(), 0);
    assistant_capture_prefix_ = std::move(prefix);
    watch_assistant_.BeginTurn();
    watch_station_policy_.KeepForConversation(esp_timer_get_time() / 1000);
    pending_assistant_listening_ = mode;
    pending_assistant_wake_word_ = wake_word;
    pending_assistant_deadline_us_ = esp_timer_get_time() + (interrupted ? 1000000LL : 10000000LL);
    StartPendingWatchListening();
    return pending_assistant_listening_.has_value() || assistant_session_.IsActive(assistant_session_.Token());
}

void Application::PollWatchSpeechInterrupt() {
    if (!watch_speech_interrupt_) return;
    const auto generation = assistant_session_.Token();
    if (GetDeviceState() != kDeviceStateSpeaking || !assistant_session_.IsActive(generation) ||
        !audio_service_.IsSpeechMonitorCurrent(generation) || !AllowsAssistantSession()) {
        watch_speech_interrupt_->Cancel();
        return;
    }
    std::vector<int16_t> prefix;
    if (watch_speech_interrupt_->Take(generation, esp_timer_get_time() / 1000, prefix)) {
        // RequestWatchListening rotates the protocol turn before admitting PCM;
        // it retains the existing recording/transfer ownership checks.
        RequestWatchListening(kListeningModeManualStop, "", 0, std::move(prefix));
    }
}
