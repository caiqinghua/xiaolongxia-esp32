#include "application.h"
#include "wifi_station.h"
#include "watch/watch_wifi_gate.h"
#include <array>
#include <cassert>
#include <iostream>

int64_t speech_test_now_us = 0;

static void Qualify(Application& app) {
    assert(app.watch_speech_interrupt_->Begin(1, 0));
    std::array<int16_t, 320> pcm;
    pcm.fill(17);
    for (unsigned i = 0; i < 12; ++i) {
        speech_test_now_us = (300 + i * 20) * 1000;
        assert(app.watch_speech_interrupt_->Feed(1, pcm.data(), pcm.size(), true,
               speech_test_now_us / 1000) == (i == 11));
    }
}

int main() {
    {
        Application app;
        assert(!app.RequestWatchListening(kListeningModeManualStop, "", 0, {1}));
        Qualify(app);
        app.PollWatchSpeechInterrupt();
        assert(app.requests == 1 && app.captured.size() == 3840);
        assert(app.state == kDeviceStateListening && app.assistant_session_.token == 2);
        assert(app.assistant_wake_turn_ && app.pending_assistant_wake_word_.empty());
        assert(app.pending_assistant_deadline_us_ == static_cast<uint64_t>(speech_test_now_us + 1000000));
        app.PollWatchSpeechInterrupt();
        assert(app.requests == 1);
    }
    for (int failure = 0; failure < 6; ++failure) {
        Application app;
        Qualify(app);
        if (failure == 0) app.state = kDeviceStateIdle;
        if (failure == 1) app.assistant_session_.active = false;
        if (failure == 2) app.audio_service_.token = 0;
        if (failure == 3) app.allowed = false;
        if (failure == 4) ++app.assistant_session_.token;
        if (failure == 5) speech_test_now_us += 201000;
        app.PollWatchSpeechInterrupt();
        assert(!app.requests && app.captured.empty());
        assert(app.watch_speech_interrupt_->Begin(3, speech_test_now_us / 1000));
    }
    {
        Application app;
        Qualify(app);
        app.request_accepted = false;
        app.PollWatchSpeechInterrupt();
        app.PollWatchSpeechInterrupt();
        assert(app.requests == 1 && app.captured.empty());
    }
    for (int failure = 0; failure < 11; ++failure) {
        Application app;
        Qualify(app);
        if (failure == 0) app.protocol_->supported = false;
        if (failure == 1) app.protocol_->suspend_ok = false;
        if (failure == 2) app.assistant_session_.next_allowed = false;
        if (failure == 3) app.assistant_stop_listening_pending_ = true;
        if (failure == 4) app.watch_cloud_authorization_->usable = false;
        if (failure == 5) app.watch_cloud_task_.active = true;
        if (failure == 6) app.watch_network_preparing_ = true;
        if (failure == 7) app.reset_protocol_pending_ = true;
        if (failure == 8) WifiStation::GetInstance().connected = false;
        if (failure == 9) GetWatchWifiGate().allowed = false;
        if (failure == 10) app.watch_capture_guard_ready_ = false;
        app.PollWatchSpeechInterrupt();
        assert(app.requests == 0 && app.captured.empty());
        if (failure >= 1 && failure <= 3) assert(!app.assistant_session_.active);
        WifiStation::GetInstance().connected = true;
        GetWatchWifiGate().allowed = true;
    }
    {
        Application app;
        assert(!app.RequestWatchListening(kListeningModeManualStop, "", 17, {1}));
        assert(!app.RequestWatchListening(kListeningModeManualStop, "wake", 0, {1}));
        assert(!app.RequestWatchListening(kListeningModeManualStop, "", 0, std::vector<int16_t>(16001)));
        app.state = kDeviceStateIdle;
        assert(!app.RequestWatchListening(kListeningModeManualStop, "", 0, {1}));
        // Ordinary manual turns retain the existing longer connection deadline.
        assert(app.RequestWatchListening(kListeningModeManualStop));
        assert(!app.assistant_wake_turn_ && app.captured.empty());
        assert(app.pending_assistant_deadline_us_ == static_cast<uint64_t>(speech_test_now_us + 10000000));
    }
    {
        Application app;
        app.watch_cloud_authorization_->settings.wake = false;
        assert(!app.RequestWatchListening(kListeningModeManualStop, "wake"));
        // Speech interruption does not impersonate a wake word or require a
        // different activation mode for an already-authorized conversation.
        Qualify(app);
        app.PollWatchSpeechInterrupt();
        assert(app.requests == 1 && app.assistant_wake_turn_);
    }
    std::cout << "PASS: owner speech interruption dispatch, stale session and preemption admission\n";
}
