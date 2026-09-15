#include "asset_release.h"
#include "audio/assistant_model_bundle.h"
#include "firmware_release.h"
#include "signed_release_json.h"

#include <set>

namespace asset_release {
namespace {
using namespace signed_release_json;
constexpr uint32_t kMaximumSequence = 2147483647;

Json ParseIndex(std::string_view bytes) {
    const auto invalid = [] { return Json(nullptr, cJSON_Delete); };
    if (bytes.empty() || bytes.size() > 64 * 1024 || bytes.find('\0') != std::string_view::npos ||
        bytes.find("\\u0000") != std::string_view::npos) return invalid();
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char ch : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 16) return invalid(); }
        else if (ch == '}' || ch == ']') { if (!depth) return invalid(); --depth; }
    }
    if (depth || quoted) return invalid();
    const std::string terminated(bytes);
    Json root(cJSON_ParseWithLengthOpts(terminated.c_str(), terminated.size() + 1, nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(root.get())) return invalid();
    std::set<std::string_view> names;
    for (auto* child = root->child; child; child = child->next)
        if (!child->string || !names.insert(child->string).second) return invalid();
    auto* version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !Token(Text(root.get(), "srmodels"), 31)) return invalid();
    return root;
}

bool HasWakeModel(const uint8_t* data, size_t size, std::string_view wake_model) {
    if (!audream::ValidateAssistantModelBundle(data, size)) return false;
    const auto models = audream::ReadBundleU32(data);
    size_t cursor = 4;
    bool found = false;
    std::set<std::string_view> names;
    for (uint32_t i = 0; i < models; ++i) {
        const std::string_view name(reinterpret_cast<const char*>(data + cursor));
        if (!Token(name, 31) || !names.insert(name).second) return false;
        found = found || name == wake_model;
        const auto files = audream::ReadBundleU32(data + cursor + 32);
        cursor += 36;
        std::set<std::string_view> file_names;
        for (uint32_t j = 0; j < files; ++j, cursor += 40) {
            const std::string_view file(reinterpret_cast<const char*>(data + cursor));
            if (!Token(file, 31) || !file_names.insert(file).second) return false;
        }
    }
    return found;
}
}  // namespace

std::optional<Verified> Verify(std::string_view envelope, const Compatibility& compatibility,
                               uint32_t confirmed_sequence, std::string_view public_key_hex) {
    using namespace signed_release_json;
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> key{};
    std::array<unsigned char, crypto_sign_BYTES> signature{};
    if (sodium_init() < 0 || !Model(compatibility.model) || !Token(compatibility.sr_abi, 63) ||
        !Token(compatibility.wake_model, 31) || !compatibility.firmware_sequence ||
        compatibility.firmware_sequence > kMaximumSequence || confirmed_sequence > kMaximumSequence ||
        !Hex(public_key_hex, key.data(), key.size()) || sodium_is_zero(key.data(), key.size())) return std::nullopt;
    auto outer = ParseFlatStrings(envelope, 2);
    if (!outer || !Hex(Text(outer.get(), "signature"), signature.data(), signature.size())) return std::nullopt;
    const auto payload = Text(outer.get(), "payload");
    if (payload.empty() || payload.size() > 2048) return std::nullopt;
    const auto signed_bytes = std::string(kSignatureDomain) + std::string(payload);
    if (crypto_sign_verify_detached(signature.data(), reinterpret_cast<const unsigned char*>(signed_bytes.data()),
                                    signed_bytes.size(), key.data()) != 0) return std::nullopt;
    auto root = ParseFlatStrings(payload, 12);
    if (!root || Text(root.get(), "schema") != "audream-assets-v1" || Text(root.get(), "format") != "mmap-v1" ||
        Text(root.get(), "model") != compatibility.model || Text(root.get(), "sr_abi") != compatibility.sr_abi ||
        Text(root.get(), "wake_model") != compatibility.wake_model || !Token(Text(root.get(), "version"), 31))
        return std::nullopt;
    Verified result;
    if (!Decimal(Text(root.get(), "sequence"), kMaximumSequence, result.sequence_) ||
        result.sequence_ <= confirmed_sequence ||
        !Decimal(Text(root.get(), "firmware_min"), kMaximumSequence, result.firmware_min_) ||
        !Decimal(Text(root.get(), "firmware_max"), kMaximumSequence, result.firmware_max_) ||
        result.firmware_min_ > compatibility.firmware_sequence || result.firmware_max_ < compatibility.firmware_sequence ||
        !Decimal(Text(root.get(), "size"), kMaximumBundleBytes, result.size_) || result.size_ < 58 ||
        !Hex(Text(root.get(), "sha256"), result.digest_.data(), result.digest_.size()) ||
        !firmware_release::IsHttpsUrl(Text(root.get(), "url"))) return std::nullopt;
    result.url_ = Text(root.get(), "url");
    result.version_ = Text(root.get(), "version");
    result.wake_model_ = compatibility.wake_model;
    return result;
}

bool ValidateBundleStructure(const void* bytes, size_t size, std::string_view wake_model) {
    if (!bytes || size < 12 || size > kMaximumBundleBytes ||
        audream::ReadBundleU32(static_cast<const uint8_t*>(bytes)) > 1024 ||
        !signed_release_json::Token(wake_model, 31) ||
        !audream::ValidateMappedAssetBundle(bytes, size)) return false;
    const auto* data = static_cast<const uint8_t*>(bytes);
    const size_t count = audream::ReadBundleU32(data);
    const size_t length = audream::ReadBundleU32(data + 8);
    if (length != size - 12 || count > 1024) return false;
    uint32_t checksum = 0;
    for (size_t i = 12; i < size; ++i) checksum = (checksum + data[i]) & 0xffff;
    if (checksum != audream::ReadBundleU32(data + 4)) return false;
    std::string_view index;
    std::set<std::string_view> names;
    for (size_t i = 0; i < count; ++i) {
        const auto* entry = data + 12 + i * 44;
        const std::string_view name(reinterpret_cast<const char*>(entry));
        if (!signed_release_json::Token(name, 31) || !names.insert(name).second) return false;
        if (name == "index.json") {
            const auto* payload = data + 12 + count * 44 + audream::ReadBundleU32(entry + 36) + 2;
            index = {reinterpret_cast<const char*>(payload), audream::ReadBundleU32(entry + 32)};
        }
    }
    auto root = ParseIndex(index);
    if (!root) return false;
    const auto model_file = signed_release_json::Text(root.get(), "srmodels");
    for (size_t i = 0; i < count; ++i) {
        const auto* entry = data + 12 + i * 44;
        if (model_file != reinterpret_cast<const char*>(entry)) continue;
        const auto* payload = data + 12 + count * 44 + audream::ReadBundleU32(entry + 36) + 2;
        return HasWakeModel(payload, audream::ReadBundleU32(entry + 32), wake_model);
    }
    return false;
}

bool ValidateBundle(const Verified& release, const void* bytes, size_t size) {
    if (!bytes || size != release.Size() || sodium_init() < 0) return false;
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    if (crypto_hash_sha256(digest.data(), static_cast<const unsigned char*>(bytes), size) != 0 ||
        sodium_memcmp(digest.data(), release.Digest().data(), digest.size()) != 0) return false;
    return ValidateBundleStructure(bytes, size, release.WakeModel());
}

bool IsModelOnlyBundle(const void* bytes, size_t size) {
    if (!bytes || size < 12 || size > kMaximumBundleBytes ||
        !audream::ValidateMappedAssetBundle(bytes, size)) return false;
    const auto* data = static_cast<const uint8_t*>(bytes);
    if (audream::ReadBundleU32(data) != 2) return false;
    std::string_view index, model_name;
    for (size_t i = 0; i < 2; ++i) {
        const auto* entry = data + 12 + i * 44;
        const std::string_view name(reinterpret_cast<const char*>(entry));
        if (name == "index.json") {
            if (!index.empty()) return false;
            const auto* payload = data + 12 + 2 * 44 + audream::ReadBundleU32(entry + 36) + 2;
            index = {reinterpret_cast<const char*>(payload), audream::ReadBundleU32(entry + 32)};
        } else model_name = name;
    }
    auto root = ParseIndex(index);
    if (!root || signed_release_json::Text(root.get(), "srmodels") != model_name) return false;
    for (auto* child = root->child; child; child = child->next) {
        const std::string_view name(child->string);
        if (name != "version" && name != "srmodels") return false;
    }
    return true;
}
}  // namespace asset_release
