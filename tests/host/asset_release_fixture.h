#pragma once
#include "asset_release.h"
#include "firmware_release_fixture.h"
#include <algorithm>
#include <vector>

namespace asset_fixture {
using namespace release_fixture;
inline constexpr auto kWake = "wn9_xiaolongxiaolong_tts";
inline constexpr asset_release::Compatibility kCompatibility{"test-watch", "esp-sr-2.3.0-esp32s3", kWake, 7};
using Bytes = std::vector<uint8_t>;
inline void U32(Bytes& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<uint8_t>(value >> (i * 8));
}
inline void Name(Bytes& bytes, size_t offset, std::string_view name) {
    assert(name.size() < 32);
    std::fill_n(bytes.begin() + offset, 32, 0);
    std::copy(name.begin(), name.end(), bytes.begin() + offset);
}
inline Bytes Model(std::string_view wake = kWake) {
    Bytes bytes(81);
    U32(bytes, 0, 1); Name(bytes, 4, wake); U32(bytes, 36, 1);
    Name(bytes, 40, "wn9_data"); U32(bytes, 72, 80); U32(bytes, 76, 1);
    bytes[80] = 42;
    return bytes;
}
inline void Checksum(Bytes& bytes) {
    uint32_t checksum = 0;
    for (size_t i = 12; i < bytes.size(); ++i) checksum = (checksum + bytes[i]) & 0xffff;
    U32(bytes, 4, checksum);
}
inline Bytes Bundle(std::string_view index = R"({"version":1,"srmodels":"srmodels.bin"})",
             std::string_view wake = kWake) {
    auto model = Model(wake);
    Bytes bytes(12 + 88);
    U32(bytes, 0, 2);
    Name(bytes, 12, "index.json"); U32(bytes, 44, index.size()); U32(bytes, 48, 0);
    Name(bytes, 56, "srmodels.bin"); U32(bytes, 88, model.size()); U32(bytes, 92, index.size() + 2);
    bytes.insert(bytes.end(), {'Z', 'Z'});
    bytes.insert(bytes.end(), index.begin(), index.end());
    bytes.insert(bytes.end(), {'Z', 'Z'});
    bytes.insert(bytes.end(), model.begin(), model.end());
    U32(bytes, 8, bytes.size() - 12); Checksum(bytes);
    return bytes;
}
inline std::string Payload(const Bytes& bytes) {
    std::array<unsigned char, 32> hash{};
    assert(crypto_hash_sha256(hash.data(), bytes.data(), bytes.size()) == 0);
    auto* root = cJSON_CreateObject();
    for (const auto& item : std::array<std::pair<const char*, std::string>, 12>{{
            {"schema", "audream-assets-v1"}, {"format", "mmap-v1"}, {"model", "test-watch"},
            {"version", "1.0.0"}, {"sequence", "8"}, {"size", std::to_string(bytes.size())},
            {"sha256", Hex(hash.data(), hash.size())}, {"url", "https://updates.example/assets.bin"},
            {"firmware_min", "6"}, {"firmware_max", "9"}, {"sr_abi", std::string(kCompatibility.sr_abi)},
            {"wake_model", kWake}}}) cJSON_AddStringToObject(root, item.first, item.second.c_str());
    return Print(root);
}
inline auto Verify(std::string_view envelope) {
    return asset_release::Verify(envelope, kCompatibility, 7, PublicKey());
}
inline auto Sign(std::string_view payload) { return Seal(payload, asset_release::kSignatureDomain); }
inline bool Accept(const Bytes& bytes) {
    const auto release = Verify(Sign(Payload(bytes)));
    return release && asset_release::ValidateBundle(*release, bytes.data(), bytes.size());
}
}  // namespace
