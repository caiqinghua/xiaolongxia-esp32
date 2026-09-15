#include "firmware_release.h"
#include "signed_release_json.h"

#include <cJSON.h>
#include <http_parser.h>
#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace firmware_release {
using namespace signed_release_json;

bool IsHttpsUrl(std::string_view url) {
    if (url.empty() || url.size() > 512 || std::any_of(url.begin(), url.end(), [](unsigned char ch) {
        return ch <= 32 || ch >= 127 || ch == '\\';
    })) return false;
    http_parser_url parsed{};
    if (http_parser_parse_url(url.data(), url.size(), 0, &parsed) != 0) return false;
    const auto schema = parsed.field_data[UF_SCHEMA];
    return (parsed.field_set & (1 << UF_SCHEMA)) && url.substr(schema.off, schema.len) == "https" &&
        (parsed.field_set & (1 << UF_HOST)) && parsed.field_data[UF_HOST].len &&
        !(parsed.field_set & ((1 << UF_USERINFO) | (1 << UF_FRAGMENT) | (1 << UF_QUERY))) &&
        (!(parsed.field_set & (1 << UF_PORT)) || parsed.port != 0);
}

std::optional<Verified> Verify(std::string_view envelope, std::string_view model,
                               uint32_t current_sequence, std::string_view public_key_hex) {
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> key{};
    std::array<unsigned char, crypto_sign_BYTES> signature{};
    if (sodium_init() < 0 || !Model(model) || !Hex(public_key_hex, key.data(), key.size()) ||
        sodium_is_zero(key.data(), key.size())) return std::nullopt;
    auto outer = ParseFlatStrings(envelope, 2);
    if (!outer || !Hex(Text(outer.get(), "signature"), signature.data(), signature.size())) return std::nullopt;
    const auto payload = Text(outer.get(), "payload");
    if (payload.empty() || payload.size() > 2048) return std::nullopt;
    const std::string signed_bytes = std::string(kSignatureDomain) + std::string(payload);
    if (crypto_sign_verify_detached(signature.data(), reinterpret_cast<const unsigned char*>(signed_bytes.data()),
                                    signed_bytes.size(), key.data()) != 0) return std::nullopt;
    auto root = ParseFlatStrings(payload, 7);
    if (!root || Text(root.get(), "schema") != "audream-firmware-v1" || Text(root.get(), "model") != model ||
        !Token(Text(root.get(), "version"), 31)) return std::nullopt;
    Verified result;
    if (!Decimal(Text(root.get(), "sequence"), 2147483647, result.sequence_) || result.sequence_ <= current_sequence ||
        !Decimal(Text(root.get(), "size"), 32U * 1024 * 1024, result.size_) || result.size_ < 396 ||
        !Hex(Text(root.get(), "sha256"), result.digest_.data(), result.digest_.size()) ||
        !IsHttpsUrl(Text(root.get(), "url"))) return std::nullopt;
    result.url_ = Text(root.get(), "url");
    result.version_ = Text(root.get(), "version");
    result.model_ = model;
    return result;
}

std::string DiscoveryUrl(std::string_view origin, std::string_view model, uint32_t current_sequence) {
    if (!IsHttpsUrl(origin) || !Model(model) || current_sequence > 2147483647) return {};
    http_parser_url parsed{};
    if (http_parser_parse_url(origin.data(), origin.size(), 0, &parsed) != 0) return {};
    if (parsed.field_set & (1 << UF_PATH)) {
        const auto path = parsed.field_data[UF_PATH];
        if (origin.substr(path.off, path.len) != "/") return {};
        origin.remove_suffix(1);
    }
    const auto url = std::string(origin) + "/v1/firmware/releases/" + std::string(model) +
        "/after/" + std::to_string(current_sequence);
    return IsHttpsUrl(url) ? url : std::string{};
}
}  // namespace firmware_release
