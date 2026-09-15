#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace watch_voice {

constexpr size_t kMaximumResponseBytes = 16384;

struct Settings {
    bool enabled = false;
    std::string activation_mode;
    std::string language;
    std::string voice;
    bool save_transcript = false;
    std::string power_mode;
    uint64_t revision = 0;
    uint64_t applied_revision = 0;
    std::string apply_error;
    bool wake_word_enabled = false;

    bool SupportsManualRuntime() const;
    bool SupportsWakeRuntime() const;
};

struct Bootstrap {
    Settings settings;
    int protocol_version = 0;
    std::string access_token;
    std::string websocket_url;
    uint64_t expires_at_ms = 0;

    // Transport freshness only; the caller must also apply device capabilities.
    bool Usable(uint64_t now_ms) const;
};

struct PendingAcknowledgement {
    std::string device_id;
    uint64_t binding_generation = 0;
    uint64_t revision = 0;
    uint64_t voice_intent = 0;
    uint64_t audio_generation = 0;
    bool local_permission = false;
    bool enabled = false;
    std::string error;
};

struct PairingReceipt {
    uint64_t credential_version = 0;
    bool active = false;
};

// These endpoints come from trusted deployment configuration, never a BLE peer.
struct Endpoints {
    std::string api_origin;
    std::string websocket_url;
    bool Valid() const;
};

bool ValidDeviceId(std::string_view value);
bool ValidCredential(std::string_view value);
bool ValidAccessToken(std::string_view value);
bool ValidWebSocketUrl(std::string_view value);
bool DecodeSettings(std::string_view body, Settings& result);
bool DecodeBootstrap(std::string_view body, const Endpoints& endpoints, int protocol_version,
                     uint64_t request_started_ms, uint64_t now_ms, Bootstrap& result);
std::string EncodeAcknowledgement(uint64_t revision, std::string_view error = {});
std::string EncodePowerModeRequest(uint64_t revision, std::string_view power_mode);
std::string EncodePairingVerification(std::string_view user_id, std::string_view challenge_id);
bool DecodePairingReceipt(std::string_view body, std::string_view device_id, std::string_view user_id,
                          std::string_view challenge_id, PairingReceipt& result);

enum class Result { kOk, kInvalidRequest, kClockRequired, kCancelled, kTimeout, kTransport,
                    kUnauthorized, kConflict, kServer, kInvalidResponse };

// Synchronous worker-only operations. Callers must join the worker before
// releasing its radio/power lease; cancellation is cooperative between I/O calls.
class Client {
public:
    using Cancelled = std::function<bool()>;
    explicit Client(Endpoints endpoints) : endpoints_(std::move(endpoints)) {}
    Result FetchBootstrap(const std::string& device_id, const std::string& credential,
                          int protocol_version, bool clock_valid, const Cancelled& cancelled,
                          Bootstrap& result) const;
    Result FetchSettings(const std::string& device_id, const std::string& credential,
                         bool clock_valid, const Cancelled& cancelled, Settings& result) const;
    Result Acknowledge(const std::string& device_id, const std::string& credential,
                       uint64_t revision, std::string_view error, bool clock_valid,
                       const Cancelled& cancelled, Settings& result) const;
    Result UpdatePowerMode(const std::string& device_id, const std::string& credential,
                           uint64_t revision, std::string_view power_mode, bool clock_valid,
                           const Cancelled& cancelled, Settings& result) const;
    // Read-only verification before durable SaveBinding. Never activates a device.
    Result VerifyPairing(const std::string& device_id, const std::string& credential,
                         const std::string& user_id, const std::string& challenge_id, bool clock_valid,
                         const Cancelled& cancelled, PairingReceipt& result) const;
private:
    Result Request(const std::string& device_id, const std::string& credential,
                   std::string_view suffix, const std::string& payload, bool clock_valid,
                   const Cancelled& cancelled, std::string& body, uint64_t& started_ms) const;
    Endpoints endpoints_;
};

}  // namespace watch_voice
