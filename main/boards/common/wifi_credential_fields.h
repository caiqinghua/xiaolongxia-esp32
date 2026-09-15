#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

template <size_t Size>
bool CopyWifiCredentialField(uint8_t (&destination)[Size], const uint8_t* source,
                             size_t length, bool allow_empty) {
    std::memset(destination, 0, Size);
    if (length > Size || (!allow_empty && length == 0) ||
        (length != 0 && (source == nullptr || std::memchr(source, 0, length) != nullptr))) {
        return false;
    }
    if (length != 0) {
        std::memcpy(destination, source, length);
    }
    return true;
}

template <size_t Size>
std::string ReadWifiCredentialField(const uint8_t (&field)[Size]) {
    const auto* begin = reinterpret_cast<const char*>(field);
    const auto* end = static_cast<const char*>(std::memchr(field, 0, Size));
    return std::string(begin, end == nullptr ? Size : static_cast<size_t>(end - begin));
}
