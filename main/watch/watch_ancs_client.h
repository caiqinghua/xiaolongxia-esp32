#pragma once

#include "sdkconfig.h"
#include "watch_notification_state.h"
#include "watch_control_bond.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include <array>
#include <mutex>

enum class WatchControlRemovalPhase { kIdle, kRevoking, kRemovingNative, kVerifying, kComplete, kFailed };
enum class WatchControlRemovalError { kNone, kStorage, kNative, kChangedBond, kTimeout, kHostStopped };
struct WatchControlRemovalSnapshot {
    uint64_t peer_generation = 0;
    WatchControlRemovalPhase phase = WatchControlRemovalPhase::kIdle;
    WatchControlRemovalError error = WatchControlRemovalError::kNone;
    bool io_pending = false;
};
struct WatchSavedControlPeer {
    uint64_t selection = 0;
    std::array<uint8_t, 6> stored_address{};
};

// Shares the existing Bluedroid host. No radio startup, scanning, persistent
// opt-in, or implicit pairing consent is performed here.
class WatchAncsClient {
public:
    static WatchAncsClient& Get();
    bool HostStarted();
    void HostStopping();
    void HostStopped();
    bool SetEnabled(bool enabled);
    // Memory-only opt-out fence for UI callbacks. Poll/SetEnabled retire the
    // physical connection later, outside the display lock.
    void DisableCollection();
    bool Confirm(uint64_t confirmation, bool accept);
    void Poll();
    bool MarkRead(uint64_t session, uint32_t uid, uint64_t revision);
    bool RequestAppName(uint64_t session, uint32_t uid, uint64_t revision);
    bool Clear(uint64_t session);
    WatchAncsClientSnapshot Snapshot() const;
    bool NeedsResponsiveLink() const;
    // Select before watch confirmation. This selects a protocol, never grants
    // pairing or account permission. Only the native Android GATT service calls it.
    bool SelectAndroidPeer(uint16_t connection, const uint8_t* address);
    // A lease for this authenticated physical peer, not an account authorization.
    uint64_t AndroidPeerLease(uint16_t connection, const uint8_t* address);
    bool RenewAndroidPeer(uint64_t lease);
    // Local pairing controller only. This never opts into notifications and
    // cannot take over an established notification session. It does not start BLE.
    uint64_t OpenDevicePairingWindow();
    // Local Add Device entry. No cloud identity, account, Wi-Fi or wall clock.
    uint64_t OpenControlPairingWindow();
    // Memory-only UI/admission snapshot; opening rechecks the same conditions.
    bool CanOpenPairingWindow(bool control_only) const;
    bool ControlPairingWindowCurrent(uint64_t window);
    bool ConfirmControlPairing(uint64_t window, uint64_t confirmation, bool accept);
    uint64_t ControlPairingLease(uint64_t window) const;
    void CancelControlPairing(uint64_t window);
    void CloseDevicePairingWindow(uint64_t window);
    bool DevicePairingWindowCurrent(uint64_t window);
    // Native pairing GATT adapter only; connection/address come from ESP events.
    bool SelectDevicePairingPeer(uint16_t connection, const uint8_t* address);
    bool ConfirmDevicePairing(uint64_t confirmation, bool accept);
    uint64_t DevicePairingPeerLease(uint16_t connection, const uint8_t* address);
    // Local Add Device owner only, after explicit control consent. A cloud
    // credential or notification approval must never call this implicitly.
    bool AuthorizeControlPeer(uint64_t window, uint64_t lease);
    // This connection only; persistent bond restoration is a separate admission.
    uint64_t ControlPeerLease(uint16_t connection, const uint8_t* address) const;
    // Local owner only, after explicit physical confirmation for this generation.
    // Revocation is irreversible once accepted; callers must poll the result.
    bool ForgetControlPeer(uint64_t peer_generation);
    bool CanForgetControlPeer(uint64_t peer_generation) const;
    WatchControlRemovalSnapshot ControlRemovalSnapshot() const;
    // Worker-only local inspection while disconnected and notifications off.
    // Selection tokens expire and are invalidated by refresh/native lifecycle.
    bool CanInspectSavedControlPeers() const;
    bool InspectSavedControlPeers(std::vector<WatchSavedControlPeer>& peers);
    bool CanForgetSavedControlPeer(uint64_t selection) const;
    bool ForgetSavedControlPeer(uint64_t selection);
    // Memory-only final credential admission. Expiry is observed without issuing
    // BLE cleanup while the caller holds the protected binding storage lock.
    bool DevicePairingCommitAllowed(uint64_t window, uint64_t lease, uint16_t connection,
                                    const uint8_t* address) const;
    // Return true for handled security events so the BLUFI example cannot
    // auto-confirm numeric comparison or log passkeys/keys behind the UI.
    bool GapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
    void GattEvent(esp_gattc_cb_event_t event, esp_gatt_if_t interface, esp_ble_gattc_cb_param_t* param);

private:
    WatchAncsClient() = default;
    bool Peer(const uint8_t* address) const;
    void Transition(WatchAncsLinkState state);
    void Retire(WatchAncsLinkState state);
    void ResetLink();
    bool ExpirePhase();
    void ServiceChanged();
    void Pump();
    bool FindCharacteristic(uint16_t start, uint16_t end, esp_bt_uuid_t uuid, uint8_t properties, uint16_t& handle);
    void Subscribe();
    bool ConfigureSecurity();
    void ExpireHostRegistration();
    bool ConfirmLocked(uint64_t confirmation, bool accept);
    bool DevicePairingWindowValid();
    uint64_t OpenPairingWindow(bool control);
    bool PairingWindowAvailable(bool control) const;
    void EndDevicePairingWindow(WatchAncsLinkState failure);
    void PollControlBond();
    void PollControlRemoval();
    bool ControlRemovalFenced() const;
    bool CanForgetControlPeerLocked(uint64_t peer_generation) const;
    bool CanInspectSavedControlPeersLocked() const;
    bool CanForgetSavedControlPeerLocked(uint64_t selection) const;
    void InvalidateSavedControlPeers();
    mutable std::mutex mutex_;
    WatchAncsSession engine_;
    WatchAncsLinkState state_ = WatchAncsLinkState::kOffline;
    bool host_ = false, registered_ = false, enabled_ = false, connected_ = false, retiring_ = false;
    bool host_registration_pending_ = false;
    uint64_t host_registration_started_ms_ = 0;
    bool security_ready_ = false;
    bool cleanup_failed_ = false;
    bool retirement_pending_ = false;
    bool android_peer_ = false;
    bool device_pairing_peer_ = false;
    bool control_peer_authorized_ = false;
    bool control_pairing_ = false;
    bool control_bond_ready_ = false;
    bool control_bond_check_ = false;
    bool restoring_control_ = false;
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    bool control_bond_busy_ = false;
#endif
    bool clear_control_bonds_ = false;
    WatchControlRemovalSnapshot control_removal_;
    watch_control::BondFingerprint control_removal_fingerprint_{};
    std::array<uint8_t, 6> control_removal_peer_{};
    std::array<uint8_t, 6> control_removal_native_address_{};
    bool control_removal_native_pending_ = false;
    bool control_removal_busy_ = false;
    uint64_t control_removal_started_ms_ = 0;
    struct SavedControlSelection {
        WatchSavedControlPeer peer;
        watch_control::StoredControlBond bond;
    };
    std::vector<SavedControlSelection> saved_control_peers_;
    uint64_t saved_control_revision_ = 0;
    uint64_t saved_control_inspected_ms_ = 0;
    std::optional<watch_control::BondFingerprint> restoring_bond_;
    std::optional<watch_control::BondFingerprint> provisional_bond_;
    // Survives native disconnect/host teardown until the revocation is drained.
    std::optional<watch_control::BondFingerprint> revoke_bond_;
    uint64_t control_grant_window_ = 0;
    uint64_t device_pairing_started_ms_ = 0;
    uint64_t device_pairing_generation_ = 0;
    bool device_pairing_window_ = false;
    esp_gatt_if_t interface_ = ESP_GATT_IF_NONE;
    uint16_t connection_ = 0;
    uint16_t mtu_ = 23;
    esp_ble_addr_type_t address_type_ = BLE_ADDR_TYPE_PUBLIC;
    std::array<uint8_t, 6> peer_{};
    uint64_t serial_ = 0, confirmation_ = 0, phase_started_ms_ = 0, session_ = 0;
    uint64_t link_generation_ = 0;
    uint32_t passkey_ = 0;
    uint16_t ancs_start_ = 0, ancs_end_ = 0, gatt_start_ = 0, gatt_end_ = 0, control_ = 0;
    // GATT Service Changed, Data Source, Notification Source, in that order.
    std::array<uint16_t, 3> subscriptions_{};
    std::array<bool, 3> registered_notifications_{};
    struct NotifyRegistration {
        std::array<uint8_t, 6> peer;
        uint16_t handle;
        uint64_t generation;
    };
    std::optional<NotifyRegistration> registering_;
    size_t subscribing_ = 0;
    uint16_t descriptor_ = 0;
    std::deque<std::array<uint8_t, 8>> early_notifications_;
    std::optional<WatchAncsRequest> write_;
};
