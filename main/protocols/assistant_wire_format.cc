#include "assistant_wire_format.h"

#include <cstring>
#include <initializer_list>

namespace assistant_wire {
namespace {

uint32_t ReadBigEndian(const uint8_t* data, size_t bytes) {
    uint32_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value = (value << 8) | data[i];
    return value;
}

void WriteBigEndian(std::string& data, size_t offset, uint32_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        data[offset + bytes - 1 - i] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
}

bool Equals(const cJSON* value, const char* expected) {
    return cJSON_IsString(value) && std::strcmp(value->valuestring, expected) == 0;
}

bool ReadOptionalNumber(const cJSON* object, const char* key,
                        std::initializer_list<int> supported, int& result) {
    const auto value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (value == nullptr) return true;
    if (!cJSON_IsNumber(value)) return false;
    for (int candidate : supported) {
        if (value->valuedouble == candidate) {
            result = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace

bool IsSupportedVersion(int version) {
    return version >= 1 && version <= 3;
}

bool DecodeAudio(int version, const void* data, size_t size, AudioView& result) {
    result = {};
    if (!IsSupportedVersion(version) || data == nullptr) return false;
    const size_t header = version == 2 ? 16 : (version == 3 ? 4 : 0);
    if (size <= header || size - header > kMaxAudioBytes) return false;
    const auto bytes = static_cast<const uint8_t*>(data);
    uint32_t timestamp = 0;
    if (version == 2) {
        if (ReadBigEndian(bytes, 2) != 2 || ReadBigEndian(bytes + 2, 2) != 0 ||
            ReadBigEndian(bytes + 12, 4) != size - header) return false;
        timestamp = ReadBigEndian(bytes + 8, 4);
    } else if (version == 3) {
        if (bytes[0] != 0 || ReadBigEndian(bytes + 2, 2) != size - header) return false;
    }
    result = {bytes + header, size - header, timestamp};
    return true;
}

bool EncodeAudio(int version, const uint8_t* data, size_t size, uint32_t timestamp,
                 std::string& result) {
    if (!IsSupportedVersion(version) || data == nullptr || size == 0 ||
        size > kMaxAudioBytes) return false;
    const size_t header = version == 2 ? 16 : (version == 3 ? 4 : 0);
    std::string encoded(header + size, '\0');
    if (version == 2) {
        WriteBigEndian(encoded, 0, 2, 2);
        WriteBigEndian(encoded, 8, timestamp, 4);
        WriteBigEndian(encoded, 12, static_cast<uint32_t>(size), 4);
    } else if (version == 3) {
        WriteBigEndian(encoded, 2, static_cast<uint32_t>(size), 2);
    }
    std::memcpy(encoded.data() + header, data, size);
    result = std::move(encoded);
    return true;
}

cJSON* ParseJson(const char* data, size_t size) {
    if (data == nullptr || size == 0 || size > kMaxJsonBytes ||
        std::memchr(data, '\0', size) != nullptr) return nullptr;
    // Limit recursion before cJSON allocates its tree on the embedded task stack.
    size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 32) return nullptr;
        } else if (c == '}' || c == ']') {
            if (depth == 0) return nullptr;
            --depth;
        }
    }
    const char* end = nullptr;
    auto root = cJSON_ParseWithLengthOpts(data, size, &end, false);
    if (root == nullptr) return nullptr;
    while (end < data + size && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    const auto type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (end != data + size || !cJSON_IsObject(root) || !cJSON_IsString(type) ||
        type->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

bool ParseHello(const cJSON* root, int version, ServerHello& result) {
    if (!IsSupportedVersion(version) || !cJSON_IsObject(root) ||
        !Equals(cJSON_GetObjectItemCaseSensitive(root, "type"), "hello") ||
        !Equals(cJSON_GetObjectItemCaseSensitive(root, "transport"), "websocket")) return false;
    int negotiated_version = version;
    if (!ReadOptionalNumber(root, "version", {version}, negotiated_version)) return false;
    ServerHello parsed;
    const auto session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (session != nullptr) {
        if (!cJSON_IsString(session) || std::strlen(session->valuestring) > 256) return false;
        parsed.session_id = session->valuestring;
    }
    const auto audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
    const auto features = cJSON_GetObjectItemCaseSensitive(root, "features");
    if (features != nullptr) {
        if (!cJSON_IsObject(features)) return false;
        const auto turns = cJSON_GetObjectItemCaseSensitive(features, "audream_turns");
        if (turns && !cJSON_IsBool(turns)) return false;
        parsed.turns = cJSON_IsTrue(turns);
        if (parsed.turns && parsed.session_id.empty()) return false;
    }
    if (audio != nullptr) {
        if (!cJSON_IsObject(audio)) return false;
        const auto format = cJSON_GetObjectItemCaseSensitive(audio, "format");
        int channels = 1;
        if ((format != nullptr && !Equals(format, "opus")) ||
            !ReadOptionalNumber(audio, "channels", {1}, channels) ||
            !ReadOptionalNumber(audio, "sample_rate", {8000, 12000, 16000, 24000, 48000}, parsed.sample_rate) ||
            !ReadOptionalNumber(audio, "frame_duration", {5, 10, 20, 40, 60, 80, 100, 120}, parsed.frame_duration)) return false;
    }
    result = std::move(parsed);
    return true;
}

}  // namespace assistant_wire
