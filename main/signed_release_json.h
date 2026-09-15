#pragma once

#include <cJSON.h>
#include <sodium.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

// Shared bounded JSON primitives for independently domain-separated releases.
namespace signed_release_json {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

inline Json ParseFlatStrings(std::string_view body, size_t expected_fields) {
    const auto invalid = [] { return Json(nullptr, cJSON_Delete); };
    if (body.empty() || body.size() > 4096 || body.find('\0') != std::string_view::npos ||
        body.find("\\u0000") != std::string_view::npos) return invalid();
    unsigned depth = 0, fields = 0;
    bool quoted = false, escaped = false;
    // Refuse nested allocation before invoking cJSON on untrusted input.
    for (char ch : body) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '[' || ch == ']') return invalid();
        else if (ch == '{') { if (++depth > 1) return invalid(); }
        else if (ch == '}') { if (depth != 1) return invalid(); --depth; }
        else if (ch == ':' && ++fields > expected_fields) return invalid();
    }
    if (depth || quoted || fields != expected_fields) return invalid();
    const std::string terminated(body);
    Json root(cJSON_ParseWithLengthOpts(terminated.c_str(), terminated.size() + 1, nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(root.get())) return invalid();
    size_t count = 0;
    for (auto* child = root->child; child; child = child->next) {
        if (++count > expected_fields || !child->string || !cJSON_IsString(child) || !child->valuestring) return invalid();
        for (auto* previous = root->child; previous != child; previous = previous->next)
            if (std::strcmp(previous->string, child->string) == 0) return invalid();
    }
    return count == expected_fields ? std::move(root) : invalid();
}

inline std::string_view Text(const cJSON* root, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return item && cJSON_IsString(item) ? std::string_view(item->valuestring) : std::string_view{};
}

inline bool Hex(std::string_view text, unsigned char* bytes, size_t count) {
    if (text.size() != count * 2 || !std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    })) return false;
    size_t written = 0;
    return sodium_hex2bin(bytes, count, text.data(), text.size(), nullptr, &written, nullptr) == 0 && written == count;
}

inline bool Token(std::string_view text, size_t maximum) {
    return !text.empty() && text.size() <= maximum && std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '+';
    });
}

inline bool Model(std::string_view text) {
    return Token(text, 95) && ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') ||
                              (text[0] >= '0' && text[0] <= '9'));
}

inline bool Decimal(std::string_view text, uint32_t maximum, uint32_t& value) {
    value = 0;
    if (text.empty() || text.size() > 10 || text[0] == '0') return false;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<uint32_t>(ch - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return false;
        value = value * 10 + digit;
    }
    return value != 0;
}
}  // namespace signed_release_json
