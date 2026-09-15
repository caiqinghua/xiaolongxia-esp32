#pragma once
#include "audio/audio_session_coordinator.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

class AudioService {
public:
    AudioSessionCoordinator* coordinator = nullptr;
    bool start_succeeds = true;
    bool drain_succeeds = true;
    bool quiesce_succeeds = true;
    bool capturing = false;
    bool resumed_assistant = false;
    std::function<void(const uint8_t*, size_t)> callback;

    AudioSessionCoordinator* GetSessionCoordinator() const { return coordinator; }
    bool EnableLocalRecording(bool enable) {
        capturing = enable && start_succeeds;
        return !enable || start_succeeds;
    }
    bool WaitForLocalRecordingDrain(uint32_t) { return drain_succeeds; }
    bool QuiesceAssistant(uint32_t) { return quiesce_succeeds; }
    bool ResumeAssistantAudio() { resumed_assistant = true; return true; }
    void SetLocalRecordingPacketCallback(std::function<void(const uint8_t*, size_t)> value) {
        callback = std::move(value);
    }
    void EmitPacket() {
        const uint8_t packet[] = {0xf8, 0xff, 0xfe};
        if (callback) { callback(packet, sizeof(packet)); }
    }
};
