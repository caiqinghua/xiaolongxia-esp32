#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audream {

inline uint32_t ReadBundleU32(const uint8_t* data) {
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 |
        uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}

// ESP-SR's binary loader has no length argument; validate all metadata first.
inline bool ValidateAssistantModelBundle(const void* bytes, size_t size) {
    if (!bytes || size < 4) return false;
    const auto* data = static_cast<const uint8_t*>(bytes);
    const uint32_t models = ReadBundleU32(data);
    if (!models || models > 32) return false;
    size_t cursor = 4;
    size_t first_payload = size;
    for (uint32_t i = 0; i < models; ++i) {
        if (cursor > size || size - cursor < 36 || !data[cursor] ||
            !std::memchr(data + cursor, 0, 32)) return false;
        const uint32_t files = ReadBundleU32(data + cursor + 32);
        cursor += 36;
        if (!files || files > 128 || files > (size - cursor) / 40) return false;
        for (uint32_t j = 0; j < files; ++j, cursor += 40) {
            if (!data[cursor] || !std::memchr(data + cursor, 0, 32)) return false;
            const size_t offset = ReadBundleU32(data + cursor + 32);
            const size_t length = ReadBundleU32(data + cursor + 36);
            if (offset > size || !length || length > INT32_MAX || length > size - offset) return false;
            if (offset < first_payload) first_payload = offset;
        }
    }
    return first_payload >= cursor;
}

// Packed mmap assets: 12-byte header, 44-byte entries, then ZZ-prefixed payloads.
inline bool ValidateMappedAssetBundle(const void* bytes, size_t size) {
    if (!bytes || size < 12) return false;
    const auto* data = static_cast<const uint8_t*>(bytes);
    const size_t files = ReadBundleU32(data);
    const size_t length = ReadBundleU32(data + 8);
    if (length > size - 12 || !files || files > length / 44) return false;
    const size_t payload_size = length - files * 44;
    for (size_t i = 0; i < files; ++i) {
        const auto* entry = data + 12 + i * 44;
        if (!entry[0] || !std::memchr(entry, 0, 32)) return false;
        const size_t asset_size = ReadBundleU32(entry + 32);
        const size_t offset = ReadBundleU32(entry + 36);
        if (offset > payload_size || payload_size - offset < 2 ||
            asset_size > payload_size - offset - 2) return false;
        const auto* payload = data + 12 + files * 44 + offset;
        if (payload[0] != 'Z' || payload[1] != 'Z') return false;
    }
    return true;
}

}  // namespace audream
