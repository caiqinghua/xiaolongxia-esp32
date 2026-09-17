#pragma once

#include "watch_android_notifications.h"
#include "esp_gatts_api.h"
#include <array>
#include <mutex>
#include <optional>
#include <vector>

// Native AWN1 service on the existing BLUFI Bluedroid host. Bluetooth security
// comes from WatchAncsClient's shared peer handshake, not from wire session IDs.
class WatchAndroidGattServer {
public:
    struct OwnerRequest {
        uint64_t generation = 0, wire_session = 0, peer_lease = 0;
        std::array<uint8_t, 6> peer{};
        std::array<uint8_t, 32> challenge{};
        uint64_t binding_generation = 0;
    };
    static WatchAndroidGattServer& Get();
    bool HostStarted();
    void HostStopping();
    void HostStopped();
    void SetEnabled(bool enabled);
    // Memory-only opt-out; Poll retires the physical link outside UI locks.
    void DisableCollection();
    void Poll();
    bool HasPendingOwnerProof();
    // Called by the application task with a storage/power reservation, never
    // from a Bluetooth callback. Verifies against the built-in server key.
    void ProcessOwnerProof();
    std::optional<OwnerRequest> PendingOwnerAuthorization();
    // Call only after an independent trusted owner/device proof verifier accepts
    // this exact challenge. No BLE opcode can invoke this authorization directly.
    bool ApplyOwnerAuthorization(const OwnerRequest& verified_request,
                                 uint64_t binding_generation, uint32_t lease_seconds,
                                 const std::array<uint8_t, 32>& peer_proof);
    // Denial/revocation affects only the captured request, never a replacement peer.
    bool RevokeOwnerAuthorization(const OwnerRequest& request);
    WatchAndroidNotificationSnapshot Snapshot();
    bool Clear(uint64_t session);
    bool MarkRead(uint64_t session, uint32_t uid, uint64_t revision);
    bool Dismiss(uint64_t session, uint32_t uid, uint64_t revision);
    void GattEvent(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t* param);

private:
    bool Peer(uint16_t connection, const uint8_t* address) const;
    bool Secure();
    bool Expire();
    void ResetLink();
    void Retire();
    void Respond(uint32_t transaction, uint16_t handle, esp_gatt_status_t status,
                 const uint8_t* bytes = nullptr, size_t size = 0);
    std::mutex mutex_;
    WatchAndroidNotifications engine_;
    bool host_ = false, enabled_ = false, started_ = false, connected_ = false;
    bool retiring_ = false, retire_pending_ = false, subscribed_ = false, ack_pending_ = false;
    bool failed_ = false;
    esp_gatt_if_t interface_ = ESP_GATT_IF_NONE;
    std::array<uint16_t, 10> handles_{};
    uint16_t connection_ = 0, mtu_ = 23;
    std::array<uint8_t, 6> peer_{};
    uint64_t serial_ = 0, generation_ = 0, engine_session_ = 0, started_at_ = 0, ack_at_ = 0;
    uint64_t owner_deadline_ = 0, binding_generation_ = 0, proof_started_at_ = 0;
    bool request_verified_ = false, proof_complete_ = false, proof_processing_ = false;
    std::vector<uint8_t> proof_;
    std::array<uint8_t, 32> peer_proof_{};
    std::optional<OwnerRequest> request_;
};
