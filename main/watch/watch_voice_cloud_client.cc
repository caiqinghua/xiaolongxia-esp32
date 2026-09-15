#include "watch_voice_cloud.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <memory>

namespace watch_voice {
namespace {
uint64_t NowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

struct Cleanup {
    void operator()(esp_http_client* client) const {
        if (client) esp_http_client_cleanup(client);
    }
};
using Http = std::unique_ptr<esp_http_client, Cleanup>;
}  // namespace

Result Client::Request(const std::string& device_id, const std::string& credential,
                       std::string_view suffix, const std::string& payload, bool clock_valid,
                       const Cancelled& cancelled, std::string& body, uint64_t& started_ms) const {
    body.clear();
    started_ms = NowMs();
    if (!clock_valid) return Result::kClockRequired;
    if (!endpoints_.Valid() || !ValidDeviceId(device_id) || !ValidCredential(credential)) return Result::kInvalidRequest;
    const auto deadline = started_ms + 15000;
    const auto check = [&]() {
        if (cancelled && cancelled()) return Result::kCancelled;
        const auto now = NowMs();
        return now < started_ms || now >= deadline ? Result::kTimeout : Result::kOk;
    };
    auto status = check();
    if (status != Result::kOk) return status;
    const std::string url = endpoints_.api_origin + "/v1/devices/" + device_id + std::string(suffix);
    // ESP-IDF debug logging includes outgoing headers; do not expose credentials.
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = payload.empty() ? HTTP_METHOD_GET : HTTP_METHOD_POST;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.timeout_ms = 5000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    Http http(esp_http_client_init(&config));
    if (!http) return Result::kTransport;
    const auto step = [&]() {
        const auto current = check();
        if (current != Result::kOk) return current;
        const auto now = NowMs();
        if (now >= deadline || now < started_ms) return Result::kTimeout;
        const auto remaining = deadline - now;
        if (esp_http_client_set_timeout_ms(http.get(), static_cast<int>(std::min<uint64_t>(remaining, 5000))) != ESP_OK)
            return Result::kTransport;
        return Result::kOk;
    };
    const std::string authorization = "Device " + credential;
    if (esp_http_client_set_header(http.get(), "Authorization", authorization.c_str()) != ESP_OK ||
        esp_http_client_set_header(http.get(), "Accept", "application/json") != ESP_OK ||
        esp_http_client_set_header(http.get(), "Cache-Control", "no-store") != ESP_OK ||
        (!payload.empty() && esp_http_client_set_header(http.get(), "Content-Type", "application/json") != ESP_OK))
        return Result::kTransport;
    if ((status = step()) != Result::kOk) return status;
    if (esp_http_client_open(http.get(), static_cast<int>(payload.size())) != ESP_OK) {
        status = check();
        return status == Result::kOk ? Result::kTransport : status;
    }
    size_t sent = 0;
    while (sent < payload.size()) {
        if ((status = step()) != Result::kOk) return status;
        const int count = esp_http_client_write(http.get(), payload.data() + sent, static_cast<int>(payload.size() - sent));
        if (count <= 0 || static_cast<size_t>(count) > payload.size() - sent) return Result::kTransport;
        sent += static_cast<size_t>(count);
    }
    if ((status = step()) != Result::kOk) return status;
    const int64_t length = esp_http_client_fetch_headers(http.get());
    if ((status = check()) != Result::kOk) return status;
    if (length < 0) return Result::kTransport;
    const int code = esp_http_client_get_status_code(http.get());
    if (code == 401 || code == 403) return Result::kUnauthorized;
    if (code == 409) return Result::kConflict;
    if (code != 200) return Result::kServer;
    if (static_cast<uint64_t>(length) > kMaximumResponseBytes) return Result::kInvalidResponse;
    std::string received;
    char buffer[1024];
    while (true) {
        if ((status = step()) != Result::kOk) return status;
        const int capacity = static_cast<int>(std::min(sizeof(buffer), kMaximumResponseBytes - received.size() + 1));
        const int count = esp_http_client_read(http.get(), buffer, capacity);
        if (count < 0 || count > capacity) return Result::kTransport;
        if ((status = check()) != Result::kOk) return status;
        if (count == 0) {
            if (!esp_http_client_is_complete_data_received(http.get())) return Result::kTransport;
            break;
        }
        if (static_cast<size_t>(count) > kMaximumResponseBytes - received.size()) return Result::kInvalidResponse;
        received.append(buffer, static_cast<size_t>(count));
    }
    body = std::move(received);
    return Result::kOk;
}

Result Client::FetchBootstrap(const std::string& device_id, const std::string& credential,
                             int protocol_version, bool clock_valid, const Cancelled& cancelled,
                             Bootstrap& result) const {
    result = {};
    if (protocol_version < 1 || protocol_version > 3) return Result::kInvalidRequest;
    std::string body;
    uint64_t started = 0;
    const auto outcome = Request(device_id, credential, "/voice/bootstrap",
        "{\"protocol_version\":" + std::to_string(protocol_version) + "}", clock_valid, cancelled, body, started);
    if (outcome != Result::kOk) return outcome;
    if (!DecodeBootstrap(body, endpoints_, protocol_version, started, NowMs(), result)) return Result::kInvalidResponse;
    if (cancelled && cancelled()) { result = {}; return Result::kCancelled; }
    return Result::kOk;
}

Result Client::FetchSettings(const std::string& device_id, const std::string& credential,
                            bool clock_valid, const Cancelled& cancelled, Settings& result) const {
    result = {};
    std::string body;
    uint64_t started = 0;
    const auto outcome = Request(device_id, credential, "/voice-settings", {}, clock_valid, cancelled, body, started);
    if (outcome != Result::kOk) return outcome;
    if (!DecodeSettings(body, result)) return Result::kInvalidResponse;
    if (cancelled && cancelled()) { result = {}; return Result::kCancelled; }
    return Result::kOk;
}

Result Client::VerifyPairing(const std::string& device_id, const std::string& credential,
                             const std::string& user_id, const std::string& challenge_id, bool clock_valid,
                             const Cancelled& cancelled, PairingReceipt& result) const {
    result = {};
    const auto payload = EncodePairingVerification(user_id, challenge_id);
    if (payload.empty()) return Result::kInvalidRequest;
    std::string body;
    uint64_t started = 0;
    const auto outcome = Request(device_id, credential, "/pairing/verify", payload,
                                 clock_valid, cancelled, body, started);
    if (outcome != Result::kOk) return outcome;
    if (!DecodePairingReceipt(body, device_id, user_id, challenge_id, result)) return Result::kInvalidResponse;
    if (cancelled && cancelled()) { result = {}; return Result::kCancelled; }
    return Result::kOk;
}

Result Client::UpdatePowerMode(const std::string& device_id, const std::string& credential,
                               uint64_t revision, std::string_view power_mode, bool clock_valid,
                               const Cancelled& cancelled, Settings& result) const {
    result = {};
    const auto payload = EncodePowerModeRequest(revision, power_mode);
    if (payload.empty()) return Result::kInvalidRequest;
    std::string body;
    uint64_t started = 0;
    const auto outcome = Request(device_id, credential, "/voice-settings/power-mode", payload,
                                 clock_valid, cancelled, body, started);
    if (outcome != Result::kOk) return outcome;
    const auto activation = power_mode == "always_available" ? "wake_word" : "push_to_talk";
    if (!DecodeSettings(body, result) || result.revision != revision + 1 ||
        result.power_mode != power_mode || result.activation_mode != activation ||
        !result.apply_error.empty() || result.applied_revision >= result.revision) {
        result = {};
        return Result::kInvalidResponse;
    }
    if (cancelled && cancelled()) { result = {}; return Result::kCancelled; }
    return Result::kOk;
}

Result Client::Acknowledge(const std::string& device_id, const std::string& credential,
                          uint64_t revision, std::string_view error, bool clock_valid,
                          const Cancelled& cancelled, Settings& result) const {
    result = {};
    const auto payload = EncodeAcknowledgement(revision, error);
    if (payload.empty()) return Result::kInvalidRequest;
    std::string body;
    uint64_t started = 0;
    const auto outcome = Request(device_id, credential, "/voice-settings/ack", payload, clock_valid, cancelled, body, started);
    if (outcome != Result::kOk) return outcome;
    if (!DecodeSettings(body, result) || result.revision != revision || result.apply_error != error ||
        (error.empty() && result.applied_revision != revision)) {
        result = {};
        return Result::kInvalidResponse;
    }
    if (cancelled && cancelled()) { result = {}; return Result::kCancelled; }
    return Result::kOk;
}
}  // namespace watch_voice
