#pragma once

#include "esp_gatts_api.h"
#include <array>
#include <mutex>
#include <optional>
#include <string>

// Bounded BLE transport only. Key access, local consent, HTTPS verification and
// durable binding belong to the application worker, never these callbacks.
class WatchPairingGattServer {
public:
    enum class Stage : uint8_t {
        kClosed = 0, kAwaitPeer = 1, kAuthenticating = 2, kReceivingChallenge = 3,
        kChallengePending = 4, kChallengeProcessing = 5, kProofReady = 6,
        kReceivingCredential = 7, kCredentialPending = 8, kCredentialProcessing = 9, kBound = 10,
        kCredentialCommitting = 11
    };
    enum class Kind : uint8_t { kChallenge = 1, kCredential = 2 };
    struct Request {
        uint64_t window = 0, peer_lease = 0, wire_request = 0;
        Kind kind = Kind::kChallenge;
        std::string payload;
    };
    static WatchPairingGattServer& Get();
    bool HostStarted();
    void HostStopping();
    void HostStopped();
    // Requires a protected identity and an already opened native security window.
    bool OpenLocalWindow(uint64_t security_window, const std::string& canonical_device_id);
    void CloseLocalWindow(uint64_t window);
    void Poll();
    bool WindowCurrent(uint64_t window);
    bool RequestCurrent(const Request& request);
    std::optional<Request> TakePending();
    bool PublishProof(const Request& request, const std::array<uint8_t, 64>& proof);
    // Called by SaveBindingAuthorized's final guard. One-shot admission is the
    // cancellation boundary; disconnect after admission cannot undo durable NVS.
    bool AdmitCredentialCommit(const Request& request);
    // Call only after admission and checked SaveBinding success.
    bool BindingCommitted(const Request& request);
    void Reject(const Request& request);
    void GattEvent(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t* param);

private:
    bool Peer(uint16_t connection, const uint8_t* address) const;
    bool Expire();
    bool Secure();
    bool Matches(const Request& request) const;
    void Clear();
    void Retire();
    void Respond(uint32_t transaction, uint16_t handle, esp_gatt_status_t status,
                 const uint8_t* bytes = nullptr, size_t size = 0);
    std::mutex mutex_;
    bool host_ = false, started_ = false, failed_ = false, connected_ = false, retiring_ = false;
    esp_gatt_if_t interface_ = ESP_GATT_IF_NONE;
    std::array<uint16_t, 7> handles_{};
    uint16_t connection_ = 0, mtu_ = 23, total_ = 0;
    std::array<uint8_t, 6> peer_{};
    uint64_t window_ = 0, opened_ms_ = 0, lease_ = 0, request_ = 0, receiving_ms_ = 0;
    std::string device_id_, buffer_;
    std::array<uint8_t, 64> proof_{};
    Stage stage_ = Stage::kClosed;
};
