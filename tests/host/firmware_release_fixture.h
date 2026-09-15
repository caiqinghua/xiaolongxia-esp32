#pragma once
#include <cJSON.h>
#include <sodium.h>
#include <array>
#include <cassert>
#include <string>
#include <string_view>
#include "firmware_release.h"

namespace release_fixture {
inline std::string Hex(const unsigned char* bytes, size_t size) {
    std::string text(size * 2 + 1, '\0');
    sodium_bin2hex(text.data(), text.size(), bytes, size);
    text.pop_back();
    return text;
}
inline std::string Print(cJSON* value) {
    auto* text = cJSON_PrintUnformatted(value);
    assert(text);
    std::string result(text);
    cJSON_free(text);
    cJSON_Delete(value);
    return result;
}
inline std::string PublicKey() {
    std::array<unsigned char, 32> seed{}, pub{};
    std::array<unsigned char, 64> secret{};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = i;
    assert(crypto_sign_seed_keypair(pub.data(), secret.data(), seed.data()) == 0);
    return Hex(pub.data(), pub.size());
}
inline std::string Seal(std::string_view payload, std::string_view domain = firmware_release::kSignatureDomain) {
    std::array<unsigned char, 32> seed{}, pub{};
    std::array<unsigned char, 64> secret{}, signature{};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = i;
    assert(crypto_sign_seed_keypair(pub.data(), secret.data(), seed.data()) == 0);
    const auto bytes = std::string(domain) + std::string(payload);
    assert(crypto_sign_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(bytes.data()),
                                bytes.size(), secret.data()) == 0);
    auto* envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope, "payload", std::string(payload).c_str());
    cJSON_AddStringToObject(envelope, "signature", Hex(signature.data(), signature.size()).c_str());
    return Print(envelope);
}
inline std::string Payload(std::string_view body) {
    std::array<unsigned char, 32> hash{};
    assert(crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(body.data()), body.size()) == 0);
    auto* root = cJSON_CreateObject();
    for (const auto& field : std::array<std::pair<const char*, std::string>, 7>{{
            {"schema", "audream-firmware-v1"}, {"model", "test-watch"}, {"version", "3.0.0"},
            {"sequence", "8"}, {"size", std::to_string(body.size())}, {"sha256", Hex(hash.data(), hash.size())},
            {"url", "https://updates.example/firmware.bin"}}})
        cJSON_AddStringToObject(root, field.first, field.second.c_str());
    return Print(root);
}
inline std::string Change(const std::string& payload, const char* key, const std::string& value) {
    auto* root = cJSON_Parse(payload.c_str());
    assert(root);
    cJSON_ReplaceItemInObjectCaseSensitive(root, key, cJSON_CreateString(value.c_str()));
    return Print(root);
}
}  // namespace release_fixture
