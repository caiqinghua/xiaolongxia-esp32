#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

enum class WatchEncodeRole { kAssistant, kRecording, kTesting };

// Used only by the codec worker. The caller owns the native encoder and must
// wait for the worker before resetting, replacing or closing that handle.
class WatchStreamEncoder {
public:
    bool Encode(void* encoder, WatchEncodeRole role, uint64_t stream,
        const std::vector<int16_t>& pcm, size_t frame_samples, size_t output_bytes,
        std::vector<uint8_t>& packet);

private:
    void* encoder_ = nullptr;
    WatchEncodeRole role_ = WatchEncodeRole::kAssistant;
    uint64_t stream_ = 0;
    bool failed_ = false;
};
