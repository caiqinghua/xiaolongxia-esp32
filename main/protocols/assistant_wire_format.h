#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <cJSON.h>

namespace assistant_wire {

constexpr size_t kMaxAudioBytes = 65535;
constexpr size_t kMaxJsonBytes = 65536;

struct AudioView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t timestamp = 0;
};

struct ServerHello {
    int sample_rate = 24000;
    int frame_duration = 60;
    std::string session_id;
    bool turns = false;
};

bool IsSupportedVersion(int version);
bool DecodeAudio(int version, const void* data, size_t size, AudioView& result);
bool EncodeAudio(int version, const uint8_t* data, size_t size, uint32_t timestamp,
                 std::string& result);
// The caller owns the returned JSON object. Input need not be NUL-terminated.
cJSON* ParseJson(const char* data, size_t size);
// Legacy servers may omit audio parameters; supplied fields must be valid.
bool ParseHello(const cJSON* root, int version, ServerHello& result);

}  // namespace assistant_wire
