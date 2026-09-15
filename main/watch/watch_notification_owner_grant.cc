#include "watch_notification_owner_grant.h"

#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include <algorithm>
#include <cstring>

namespace watch_notification {
namespace {
struct PublicKey {
    mbedtls_pk_context context;
    PublicKey() { mbedtls_pk_init(&context); }
    ~PublicKey() { mbedtls_pk_free(&context); }
};
int Hex(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}
bool DeviceMatches(std::string_view id, const uint8_t* bytes) {
    if (id.size() != 36) return false;
    size_t byte = 0;
    for (size_t i = 0; i < id.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i++] != '-') return false;
            continue;
        }
        const int high = Hex(id[i]), low = Hex(id[i + 1]);
        if (high < 0 || low < 0 || bytes[byte++] != (high * 16 + low)) return false;
        i += 2;
    }
    return byte == 16;
}
uint64_t Little(const uint8_t* bytes, size_t size) {
    uint64_t result = 0;
    for (size_t i = 0; i < size; ++i) result |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return result;
}
}
std::optional<uint32_t> VerifyOwnerGrant(std::string_view pin, std::string_view device,
    uint64_t wire, const std::array<uint8_t, 32>& challenge, const uint8_t* proof, size_t size) {
    constexpr char domain[] = "AudreamNotifyV1";
    static_assert(sizeof(domain) == 16);
    if (!proof || size < kOwnerGrantPrefixSize + 256 || size > kOwnerGrantMaxSize ||
        !wire || wire > 0x7fffffffffffffffULL || pin.empty() || pin.size() > 800 ||
        std::memcmp(proof, domain, sizeof(domain)) || !DeviceMatches(device, proof + 16) ||
        Little(proof + 48, 8) != wire || !std::equal(challenge.begin(), challenge.end(), proof + 56)) return std::nullopt;
    const auto lease = Little(proof + 88, 4);
    const auto signature_size = Little(proof + 92, 2);
    if (!lease || lease > kOwnerGrantMaxLeaseSeconds ||
        (signature_size != 256 && signature_size != 384 && signature_size != 512) ||
        size != kOwnerGrantPrefixSize + signature_size) return std::nullopt;
    // Reject whitespace and embedded NULs even if the library's decoder accepts them.
    if (pin.size() % 4 || !std::all_of(pin.begin(), pin.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
    })) return std::nullopt;
    std::array<uint8_t, 600> der{};
    size_t der_size = 0;
    if (mbedtls_base64_decode(der.data(), der.size(), &der_size,
        reinterpret_cast<const uint8_t*>(pin.data()), pin.size()) != 0) return std::nullopt;
    PublicKey key;
    if (mbedtls_pk_parse_public_key(&key.context, der.data(), der_size) != 0 ||
        mbedtls_pk_get_type(&key.context) != MBEDTLS_PK_RSA ||
        mbedtls_pk_get_bitlen(&key.context) != signature_size * 8) return std::nullopt;
    if (mbedtls_rsa_set_padding(mbedtls_pk_rsa(key.context), MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_SHA256) != 0)
        return std::nullopt;
    std::array<uint8_t, 32> digest{};
    if (mbedtls_sha256(proof, kOwnerGrantBodySize, digest.data(), 0) != 0 ||
        mbedtls_pk_verify(&key.context, MBEDTLS_MD_SHA256, digest.data(), digest.size(),
                         proof + kOwnerGrantPrefixSize, signature_size) != 0) return std::nullopt;
    return static_cast<uint32_t>(lease);
}
std::optional<std::array<uint8_t, 32>> BuildPeerProof(std::string_view credential,
    const std::array<uint8_t, 32>& nonce, const uint8_t* proof, size_t size) {
    constexpr char domain[] = "AudreamNotifyPeerV1";
    if (!proof || size < kOwnerGrantPrefixSize + 256 || size > kOwnerGrantMaxSize ||
        credential.size() < 47 || credential.size() > 128 || credential.substr(0, 4) != "adv_") return std::nullopt;
    std::array<uint8_t, 32> key{}, result{};
    std::array<uint8_t, sizeof(domain) + 64> message{};
    std::copy_n(domain, sizeof(domain), message.begin());
    std::copy(nonce.begin(), nonce.end(), message.begin() + sizeof(domain));
    const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool success = info && mbedtls_sha256(reinterpret_cast<const uint8_t*>(credential.data()), credential.size(), key.data(), 0) == 0 &&
        mbedtls_sha256(proof, size, message.data() + sizeof(domain) + 32, 0) == 0 &&
        mbedtls_md_hmac(info, key.data(), key.size(), message.data(), message.size(), result.data()) == 0;
    mbedtls_platform_zeroize(key.data(), key.size());
    return success ? std::optional(result) : std::nullopt;
}
}  // namespace watch_notification
