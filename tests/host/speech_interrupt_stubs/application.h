#pragma once
#include "watch/watch_speech_interrupt.h"
#include "watch/watch_voice_capture.h"
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

extern int64_t speech_test_now_us;
inline int64_t esp_timer_get_time() { return speech_test_now_us; }
enum DeviceState { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking };
enum ListeningMode { kListeningModeManualStop };
enum class WatchAssistantError { kNetwork };
class Application {
public:
    std::unique_ptr<WatchSpeechInterrupt> watch_speech_interrupt_ = std::make_unique<WatchSpeechInterrupt>();
    struct Session {
        uint64_t token = 1;
        bool active = true;
        bool next_allowed = true;
        uint64_t Token() const { return token; }
        bool IsActive(uint64_t value) const { return active && value == token; }
        bool BeginNextTurn(uint64_t value) {
            if (!next_allowed || !IsActive(value)) return false;
            ++token;
            return true;
        }
    } assistant_session_;
    struct Audio {
        uint64_t token = 1;
        bool IsSpeechMonitorCurrent(uint64_t value) const { return value && value == token; }
    } audio_service_;
    DeviceState state = kDeviceStateSpeaking;
    bool allowed = true, request_accepted = true;
    unsigned requests = 0;
    std::vector<int16_t> captured;
    DeviceState GetDeviceState() const { return state; }
    bool AllowsAssistantSession() const { return allowed; }
    struct Protocol {
        bool supported = true, suspend_ok = true;
        bool SupportsConversationTurns() const { return supported; }
        bool SuspendIncomingTurn() { return suspend_ok; }
    };
    std::unique_ptr<Protocol> protocol_ = std::make_unique<Protocol>();
    struct Cloud {
        struct Settings { bool wake = true; bool SupportsWakeRuntime() const { return wake; } } settings;
        bool usable = true;
        bool Usable(uint64_t) const { return usable; }
    };
    std::unique_ptr<Cloud> watch_cloud_authorization_ = std::make_unique<Cloud>();
    struct CloudTask { bool active = false; bool Active() const { return active; } } watch_cloud_task_;
    struct Press {
        bool held = true;
        bool Held(uint64_t, uint64_t) const { return held; }
        void Retire(uint64_t) {}
    } watch_press_;
    struct Assistant { void Fail(WatchAssistantError) {} void BeginTurn() {} } watch_assistant_;
    struct Station { void KeepForConversation(uint64_t) {} } watch_station_policy_;
    WatchVoiceCapture watch_voice_capture_;
    bool watch_network_preparing_ = false, watch_capture_guard_ready_ = true, reset_protocol_pending_ = false;
    bool watch_cloud_required_ = true, assistant_stop_listening_pending_ = false;
    bool assistant_response_finished_ = false, assistant_wake_turn_ = false;
    std::atomic<uint64_t> assistant_press_token_{0}, assistant_wake_capture_generation_{0};
    uint64_t assistant_response_deadline_us_ = 0, assistant_capture_deadline_us_ = 0;
    uint64_t assistant_conversation_idle_deadline_us_ = 0, pending_assistant_deadline_us_ = 0;
    ListeningMode listening_mode_ = kListeningModeManualStop;
    std::optional<ListeningMode> pending_assistant_listening_;
    std::string pending_assistant_wake_word_;
    std::vector<int16_t> assistant_capture_prefix_;
    bool HasAssistantSession() const { return assistant_session_.active; }
    void SetDeviceState(DeviceState value) { state = value; }
    void InitializeProtocol() { protocol_ = std::make_unique<Protocol>(); }
    void EndWatchAssistantSession() {
        watch_speech_interrupt_->Cancel();
        assistant_capture_prefix_.clear();
        assistant_session_.active = false;
        pending_assistant_listening_.reset();
        state = kDeviceStateIdle;
    }
    void StartPendingWatchListening() {
        ++requests;
        if (!request_accepted) { EndWatchAssistantSession(); return; }
        captured = assistant_capture_prefix_;
        audio_service_.token = 0;
        state = kDeviceStateListening;
        assistant_session_.active = true;
        pending_assistant_listening_.reset();
    }
    bool RequestWatchListening(ListeningMode, const std::string& = "", uint64_t = 0, std::vector<int16_t> = {});
    void PollWatchSpeechInterrupt();
};
