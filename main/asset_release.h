#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace asset_release {
constexpr size_t kMaximumBundleBytes = 16U * 1024 * 1024;
constexpr std::string_view kSignatureDomain = "audream-asset-release-v1\n";

// Values must come from the running firmware and its trusted release state.
struct Compatibility {
    std::string_view model;
    std::string_view sr_abi;
    std::string_view wake_model;
    uint32_t firmware_sequence = 0;
};

class Verified {
public:
    const std::string& Url() const { return url_; }
    const std::string& Version() const { return version_; }
    const std::string& WakeModel() const { return wake_model_; }
    uint32_t Sequence() const { return sequence_; }
    uint32_t Size() const { return size_; }
    uint32_t MinimumFirmware() const { return firmware_min_; }
    uint32_t MaximumFirmware() const { return firmware_max_; }
    const std::array<unsigned char, 32>& Digest() const { return digest_; }
private:
    Verified() = default;
    friend std::optional<Verified> Verify(std::string_view, const Compatibility&, uint32_t, std::string_view);
    std::string url_, version_, wake_model_;
    uint32_t sequence_ = 0, size_ = 0, firmware_min_ = 0, firmware_max_ = 0;
    std::array<unsigned char, 32> digest_{};
};

// Fresh updates must exceed the confirmed sequence; boot rollback is a separate policy.
std::optional<Verified> Verify(std::string_view envelope, const Compatibility& compatibility,
                               uint32_t confirmed_sequence, std::string_view public_key_hex);

// Never pass a new bundle to the SDK's lengthless loader before this succeeds.
bool ValidateBundle(const Verified& release, const void* bytes, size_t size);
// Also used by the offline signer, before a release exists.
bool ValidateBundleStructure(const void* bytes, size_t size, std::string_view wake_model);
// Classification only; callers must still authenticate and validate the bundle.
bool IsModelOnlyBundle(const void* bytes, size_t size);
}  // namespace asset_release
