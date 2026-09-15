#include "ota.h"
#include "firmware_release.h"
#include "firmware_release_identity.h"
#include "sdkconfig.h"

#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <cstring>
#include <sodium.h>

namespace {
constexpr size_t kPageSize = 4096;
constexpr size_t kMinimumImageSize = sizeof(esp_image_header_t) +
    sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);

class OtaWrite {
public:
    explicit OtaWrite(bool* resources_released) : resources_released_(resources_released) {}
    OtaWrite(const OtaWrite&) = delete;
    OtaWrite& operator=(const OtaWrite&) = delete;
    ~OtaWrite() {
        if (active_ && esp_ota_abort(handle_) != ESP_OK) {
            if (resources_released_) *resources_released_ = false;
            ESP_LOGE("Ota", "OTA staging handle could not be aborted");
        }
    }
    bool Begin(const esp_partition_t* partition) {
        if (active_) return false;
        active_ = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle_) == ESP_OK;
        return active_;
    }
    bool Active() const { return active_; }
    bool Write(const void* data, size_t size) {
        return active_ && esp_ota_write(handle_, data, size) == ESP_OK;
    }
    esp_err_t Finish() {
        // ESP-IDF consumes the handle even when validation fails.
        const auto result = esp_ota_end(handle_);
        active_ = false;
        return result;
    }
private:
    esp_ota_handle_t handle_ = 0;
    bool active_ = false;
    bool* resources_released_ = nullptr;
};

struct HttpClose {
    Http& http;
    ~HttpClose() { http.Close(); }
};
}  // namespace

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int, size_t)> callback) {
    if (Board::GetInstance().GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
        ESP_LOGE("Ota", "Unsigned firmware upgrades are not allowed in watch mode");
        return false;
    }
    return UpgradeImage(firmware_url, std::move(callback), nullptr);
}

bool Ota::UpgradeSigned(const std::string& manifest_url, std::function<void(int, size_t)> callback) {
    auto checked = ReadSignedRelease(manifest_url, false, {});
    if (checked.status != firmware_release::CheckStatus::kAvailable || !checked.release) return false;
    return UpgradeImage(checked.release->Url(), std::move(callback), &*checked.release);
}

firmware_release::CheckResult Ota::CheckSignedRelease(std::function<bool()> cancelled) {
#if CONFIG_AUDREAM_WATCH_UI
    const auto* identity = firmware_release::CurrentIdentity();
    if (!identity) return {};
    const auto url = firmware_release::DiscoveryUrl(CONFIG_AUDREAM_OTA_API_ORIGIN, identity->model, identity->sequence);
    return ReadSignedRelease(url, true, cancelled);
#else
    (void)cancelled;
    return {};
#endif
}

bool Ota::UpgradeSignedEnvelope(const std::string& envelope, std::function<void(int, size_t)> callback,
                                std::function<bool()> cancelled, std::function<bool()> before_commit) {
    return InstallSignedEnvelope(envelope, std::move(callback), std::move(cancelled), std::move(before_commit)).installed;
}

firmware_release::InstallResult Ota::InstallSignedEnvelope(const std::string& envelope,
    std::function<void(int, size_t)> callback, std::function<bool()> cancelled, std::function<bool()> before_commit) {
    if (cancelled && cancelled()) return {};
#if CONFIG_AUDREAM_WATCH_UI
    const auto* identity = firmware_release::CurrentIdentity();
    if (!identity) return {};
    const auto release = firmware_release::Verify(envelope, identity->model, identity->sequence,
                                                 CONFIG_AUDREAM_OTA_PUBLIC_KEY_HEX);
    if (!release) return {};
    firmware_release::InstallResult result;
    result.installed = UpgradeImage(release->Url(), std::move(callback), &*release, cancelled, before_commit,
                                    &result.resources_released);
    return result;
#else
    (void)envelope;
    (void)callback;
    (void)before_commit;
    return {};
#endif
}

firmware_release::CheckResult Ota::ReadSignedRelease(const std::string& manifest_url, bool allow_no_update,
                                                    const std::function<bool()>& cancelled) {
    using firmware_release::CheckStatus;
    const auto failure = [](CheckStatus status) { return firmware_release::CheckResult{status, {}, {}}; };
    if (cancelled && cancelled()) return failure(CheckStatus::kCancelled);
#if CONFIG_AUDREAM_WATCH_UI
    const auto* identity = firmware_release::CurrentIdentity();
    if (!identity) return {};
    if (std::strlen(CONFIG_AUDREAM_OTA_PUBLIC_KEY_HEX) != 64 || !firmware_release::IsHttpsUrl(manifest_url)) {
        ESP_LOGE("Ota", "Signed update is not configured or the manifest URL is invalid");
        return {};
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) return failure(CheckStatus::kTransportError);
    auto http = network->CreateHttp(0);
    if (!http) return failure(CheckStatus::kTransportError);
    firmware_release::CheckResult result;
    {
        HttpClose close{*http};
        http->SetTimeout(15000);
        if (!http->Open("GET", manifest_url)) return failure(CheckStatus::kTransportError);
        if (cancelled && cancelled()) return failure(CheckStatus::kCancelled);
        if (http->GetStatusCode() == 204 && allow_no_update && http->GetBodyLength() == 0)
            return failure(CheckStatus::kNoUpdate);
        if (http->GetStatusCode() != 200) return failure(CheckStatus::kTransportError);
        const auto expected = http->GetBodyLength();
        if (!expected || expected > firmware_release::kMaximumManifestBytes) return failure(CheckStatus::kInvalidRelease);
        std::string body;
        body.reserve(expected);
        char chunk[512];
        while (true) {
            if (cancelled && cancelled()) return failure(CheckStatus::kCancelled);
            const int received = http->Read(chunk, sizeof(chunk));
            if (received < 0) return failure(CheckStatus::kTransportError);
            if (received > static_cast<int>(sizeof(chunk)) ||
                static_cast<size_t>(received) > expected - body.size()) return failure(CheckStatus::kInvalidRelease);
            if (!received) break;
            body.append(chunk, received);
        }
        if (body.size() != expected) return failure(CheckStatus::kInvalidRelease);
        result.release = firmware_release::Verify(body, identity->model, identity->sequence,
                                                  CONFIG_AUDREAM_OTA_PUBLIC_KEY_HEX);
        if (result.release) result.envelope = std::move(body);
    }
    http.reset();
    if (cancelled && cancelled()) return failure(CheckStatus::kCancelled);
    if (!result.release) {
        ESP_LOGE("Ota", "Firmware release signature or metadata was rejected");
        return failure(CheckStatus::kInvalidRelease);
    }
    result.status = CheckStatus::kAvailable;
    return result;
#else
    (void)manifest_url;
    (void)allow_no_update;
    return {};
#endif
}

bool Ota::UpgradeImage(const std::string& firmware_url, std::function<void(int, size_t)> callback,
                       const firmware_release::Verified* release, const std::function<bool()>& cancelled,
                       const std::function<bool()>& before_commit, bool* resources_released) {
    if (cancelled && cancelled()) return false;
    const auto* partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition) {
        ESP_LOGE("Ota", "No OTA staging partition is available");
        return false;
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) return false;
    auto http = network->CreateHttp(0);
    if (!http) return false;
    HttpClose close{*http};
    http->SetTimeout(15000);
    if (!http->Open("GET", firmware_url) || http->GetStatusCode() != 200) {
        ESP_LOGE("Ota", "Firmware download could not be opened");
        return false;
    }
    const size_t length = http->GetBodyLength();
    if (release && (length != release->Size() || length < kMinimumImageSize + sizeof(firmware_release::ImageIdentity))) {
        ESP_LOGE("Ota", "Firmware length does not match the signed release");
        return false;
    }
    if (length < kMinimumImageSize || length > partition->size) {
        ESP_LOGE("Ota", "Firmware length is missing, too small or exceeds the OTA partition");
        return false;
    }
    std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
        static_cast<char*>(heap_caps_malloc(kPageSize, MALLOC_CAP_INTERNAL)), heap_caps_free);
    if (!buffer) return false;
    OtaWrite writer(resources_released);
    crypto_hash_sha256_state digest_state{};
    if (release && (sodium_init() < 0 || crypto_hash_sha256_init(&digest_state) != 0)) return false;
    size_t buffered = 0, total = 0, recent = 0;
    auto last_progress = esp_timer_get_time();
    size_t speed = 0;
    while (true) {
        if (cancelled && cancelled()) return false;
        const size_t capacity = kPageSize - buffered;
        const int received = http->Read(buffer.get() + buffered, capacity);
        if (cancelled && cancelled()) return false;
        if (received < 0 || static_cast<size_t>(received) > capacity) {
            ESP_LOGE("Ota", "Firmware download read failed");
            return false;
        }
        if (static_cast<size_t>(received) > length - total) {
            ESP_LOGE("Ota", "Firmware download exceeds its declared length");
            return false;
        }
        const bool eof = received == 0;
        if (eof && total != length) {
            ESP_LOGE("Ota", "Firmware download ended before its declared length");
            return false;
        }
        if (release && received && crypto_hash_sha256_update(&digest_state,
                reinterpret_cast<const unsigned char*>(buffer.get() + buffered), received) != 0) return false;
        buffered += received;
        total += received;
        recent += received;

        if (buffered == kPageSize || (eof && buffered)) {
            if (release && !writer.Active()) {
                esp_app_desc_t descriptor{};
                std::memcpy(&descriptor, buffer.get() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
                            sizeof(descriptor));
                if (!std::memchr(descriptor.version, '\0', sizeof(descriptor.version)) ||
                    release->Version() != descriptor.version) {
                    ESP_LOGE("Ota", "Firmware image version does not match the signed release");
                    return false;
                }
                firmware_release::ImageIdentity image_identity{};
                std::memcpy(&image_identity, buffer.get() + kMinimumImageSize, sizeof(image_identity));
                if (std::memcmp(image_identity.magic, "AUDOTA1", sizeof(image_identity.magic)) != 0 ||
                    image_identity.sequence != release->Sequence() ||
                    !std::memchr(image_identity.model, '\0', sizeof(image_identity.model)) ||
                    release->Model() != image_identity.model) {
                    ESP_LOGE("Ota", "Firmware image identity does not match the signed release");
                    return false;
                }
            }
            if (!writer.Active() && !writer.Begin(partition)) {
                ESP_LOGE("Ota", "OTA staging could not begin");
                return false;
            }
            if (cancelled && cancelled()) return false;
            if (!writer.Write(buffer.get(), buffered)) {
                ESP_LOGE("Ota", "OTA staging write failed");
                return false;
            }
            buffered = 0;
        }
        const auto now = esp_timer_get_time();
        if (now - last_progress >= 1000000 || eof) {
            if (now > last_progress) speed = static_cast<uint64_t>(recent) * 1000000 / (now - last_progress);
            if (callback) callback(static_cast<int>(std::min<uint64_t>(99, static_cast<uint64_t>(total) * 100 / length)), speed);
            last_progress = now;
            recent = 0;
        }
        if (eof) break;
    }
    if (!writer.Active()) return false;
    if (release) {
        std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
        if (crypto_hash_sha256_final(&digest_state, digest.data()) != 0 ||
            sodium_memcmp(digest.data(), release->Digest().data(), digest.size()) != 0) {
            ESP_LOGE("Ota", "Firmware bytes do not match the signed release digest");
            return false;
        }
    }
    if (cancelled && cancelled()) return false;
    const auto verified = writer.Finish();
    if (verified != ESP_OK) {
        ESP_LOGE("Ota", "Firmware image verification failed: %s", esp_err_to_name(verified));
        return false;
    }
    // The owner serializes cancellation against this final admission. Once
    // admitted, a late cancellation must not hide a changed boot partition.
    if ((cancelled && cancelled()) || (before_commit && !before_commit())) return false;
    if (esp_ota_set_boot_partition(partition) != ESP_OK) {
        ESP_LOGE("Ota", "Verified firmware could not be selected for boot");
        return false;
    }
    if (callback) callback(100, speed);
    ESP_LOGI("Ota", "Firmware staging and boot selection completed");
    return true;
}
