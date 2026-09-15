#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace firmware_release {
constexpr size_t kMaximumManifestBytes = 4096;
constexpr std::string_view kSignatureDomain = "audream-firmware-release-v1\n";

bool IsHttpsUrl(std::string_view url);
std::string DiscoveryUrl(std::string_view origin, std::string_view model, uint32_t current_sequence);

class Verified {
public:
    const std::string& Url() const { return url_; }
    const std::string& Version() const { return version_; }
    const std::string& Model() const { return model_; }
    uint32_t Size() const { return size_; }
    uint32_t Sequence() const { return sequence_; }
    const std::array<unsigned char, 32>& Digest() const { return digest_; }
private:
    Verified() = default;
    friend std::optional<Verified> Verify(std::string_view, std::string_view, uint32_t, std::string_view);
    std::string url_, version_, model_;
    uint32_t size_ = 0, sequence_ = 0;
    std::array<unsigned char, 32> digest_{};
};

// The public key and minimum sequence come from trusted firmware, not HTTP.
std::optional<Verified> Verify(std::string_view envelope, std::string_view model,
                               uint32_t current_sequence, std::string_view public_key_hex);

enum class CheckStatus { kAvailable, kNoUpdate, kNotConfigured, kTransportError, kInvalidRelease, kCancelled };
struct CheckResult {
    CheckStatus status = CheckStatus::kNotConfigured;
    std::optional<Verified> release;
    // Preserve the approved signed bytes instead of re-fetching a changing latest URL.
    std::string envelope;
};
struct InstallResult {
    bool installed = false;
    bool resources_released = true;
};
}  // namespace firmware_release
