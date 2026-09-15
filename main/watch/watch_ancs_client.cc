#include "watch_ancs_client.h"
#include "esp_timer.h"
#include "esp_gatt_common_api.h"
#include <algorithm>
#include <cstring>
#include <limits>

bool WatchAncsClient::NeedsResponsiveLink() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || retiring_) return false;
    if (restoring_control_ || control_bond_check_ || engine_.HasPendingRequests()) return true;
    switch (state_) {
        case WatchAncsLinkState::kPeerConsent:
        case WatchAncsLinkState::kOpening:
        case WatchAncsLinkState::kNegotiating:
        case WatchAncsLinkState::kPairing:
        case WatchAncsLinkState::kConfirmNumber:
        case WatchAncsLinkState::kDiscovering:
        case WatchAncsLinkState::kSubscribing:
        case WatchAncsLinkState::kDevicePairingReady:
            return true;
        default:
            return false;
    }
}
namespace {
constexpr uint16_t kAppId = 0xada1;
constexpr uint64_t kPhaseTimeoutMs = 60000;
constexpr uint64_t kDevicePairingWindowMs = 120000;
constexpr uint8_t kAncs[]{0xd0,0x00,0x2d,0x12,0x1e,0x4b,0x0f,0xa4,0x99,0x4e,0xce,0xb5,0x31,0xf4,0x05,0x79};
constexpr uint8_t kSource[]{0xbd,0x1d,0xa2,0x99,0xe6,0x25,0x58,0x8c,0xd9,0x42,0x01,0x63,0x0d,0x12,0xbf,0x9f};
constexpr uint8_t kControl[]{0xd9,0xd9,0xaa,0xfd,0xbd,0x9b,0x21,0x98,0xa8,0x49,0xe1,0x45,0xf3,0xd8,0xd1,0x69};
constexpr uint8_t kData[]{0xfb,0x7b,0x7c,0xce,0x6a,0xb3,0x44,0xbe,0xb5,0x4b,0xd6,0x24,0xe9,0xc6,0xea,0x22};
uint64_t Now() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
esp_bt_uuid_t Uuid(uint16_t value) {
    esp_bt_uuid_t uuid{}; uuid.len = ESP_UUID_LEN_16; uuid.uuid.uuid16 = value; return uuid;
}
esp_bt_uuid_t Uuid(const uint8_t (&value)[16]) {
    esp_bt_uuid_t uuid{}; uuid.len = ESP_UUID_LEN_128; std::copy_n(value, 16, uuid.uuid.uuid128); return uuid;
}
bool IsUuid(const esp_bt_uuid_t& uuid, const uint8_t (&value)[16]) {
    return uuid.len == ESP_UUID_LEN_128 && std::equal(value, value + 16, uuid.uuid.uuid128);
}
void GattCallback(esp_gattc_cb_event_t event, esp_gatt_if_t interface, esp_ble_gattc_cb_param_t* param) {
    WatchAncsClient::Get().GattEvent(event, interface, param);
}
}

WatchAncsClient& WatchAncsClient::Get() { static WatchAncsClient instance; return instance; }
bool WatchAncsClient::Peer(const uint8_t* address) const {
    return connected_ && address && std::equal(peer_.begin(), peer_.end(), address);
}
void WatchAncsClient::Transition(WatchAncsLinkState state) {
    state_ = cleanup_failed_ ? WatchAncsLinkState::kFailed : state;
    phase_started_ms_ = Now(); confirmation_ = 0; passkey_ = 0;
    if (state_ == WatchAncsLinkState::kPeerConsent || state_ == WatchAncsLinkState::kConfirmNumber) {
        if (++serial_ == 0) ++serial_;
        confirmation_ = serial_;
    }
}
void WatchAncsClient::ResetLink() {
    InvalidateSavedControlPeers();
    retirement_pending_ = false;
    android_peer_ = false;
    device_pairing_peer_ = false;
    control_peer_authorized_ = false;
    control_pairing_ = false;
    control_bond_ready_ = control_bond_check_ = restoring_control_ = false;
    restoring_bond_.reset();
    provisional_bond_.reset();
    control_grant_window_ = 0;
    if (host_ && registered_) {
        for (size_t i = 0; i < subscriptions_.size(); ++i) {
            if (registered_notifications_[i] && esp_ble_gattc_unregister_for_notify(interface_, peer_.data(), subscriptions_[i]) != ESP_OK)
                cleanup_failed_ = true;
        }
    }
    registered_notifications_.fill(false);
    engine_.EndSession(session_); session_ = 0;
    early_notifications_.clear(); write_.reset();
    security_ready_ = false; ancs_start_ = ancs_end_ = gatt_start_ = gatt_end_ = control_ = descriptor_ = 0;
    mtu_ = 23;
    subscriptions_.fill(0); subscribing_ = 0; confirmation_ = 0; passkey_ = 0;
}
void WatchAncsClient::Retire(WatchAncsLinkState state) {
    retirement_pending_ = false;
    device_pairing_window_ = false;
    ResetLink(); Transition(state);
    if (connected_ && !retiring_) {
        retiring_ = true;
        // A physical disconnect is required before reusing the logical session.
        // If it fails, keep the link retired until a disconnect or host stop.
        (void)esp_ble_gap_disconnect(peer_.data());
    }
}
bool WatchAncsClient::ExpirePhase() {
    if (device_pairing_window_ && !DevicePairingWindowValid()) return true;
    if (!host_ || (!enabled_ && !device_pairing_peer_ && !control_pairing_ && !restoring_control_) || retiring_ || !connected_ ||
        state_ == WatchAncsLinkState::kReady || state_ == WatchAncsLinkState::kDevicePairingReady ||
        state_ == WatchAncsLinkState::kControlReady ||
        state_ == WatchAncsLinkState::kOffline || state_ == WatchAncsLinkState::kDisabled ||
        state_ == WatchAncsLinkState::kUnavailable || state_ == WatchAncsLinkState::kFailed) return false;
    const auto now = Now();
    if (now >= phase_started_ms_ && now - phase_started_ms_ < kPhaseTimeoutMs) return false;
    Retire(WatchAncsLinkState::kFailed); return true;
}
bool WatchAncsClient::DevicePairingWindowValid() {
    if (!device_pairing_window_) return false;
    const auto now = Now();
    if (now >= device_pairing_started_ms_ && now - device_pairing_started_ms_ < kDevicePairingWindowMs) return true;
    EndDevicePairingWindow(WatchAncsLinkState::kFailed);
    return false;
}
void WatchAncsClient::EndDevicePairingWindow(WatchAncsLinkState failure) {
    const bool control = control_pairing_;
    control_pairing_ = false;
    device_pairing_window_ = false;
    if (control_peer_authorized_ && security_ready_ && connected_ && !retiring_ && !cleanup_failed_) {
        device_pairing_peer_ = false;
        Transition(WatchAncsLinkState::kControlReady);
    } else if (device_pairing_peer_ || (control && connected_)) {
        Retire(failure);
    }
}
void WatchAncsClient::ServiceChanged() {
    Retire(WatchAncsLinkState::kUnavailable);
    if (esp_ble_gattc_cache_refresh(peer_.data()) != ESP_OK) {
        cleanup_failed_ = true; Transition(WatchAncsLinkState::kFailed);
    }
}
bool WatchAncsClient::ConfigureSecurity() {
    uint8_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    uint8_t capability = ESP_IO_CAP_IO;
    uint8_t key_size = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t only = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    return esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth)) == ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &capability, sizeof(capability)) == ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)) == ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys)) == ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys)) == ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &only, sizeof(only)) == ESP_OK;
}
bool WatchAncsClient::HostStarted() {
    std::lock_guard<std::mutex> lock(mutex_);
    ExpireHostRegistration();
    if (host_) return (registered_ || host_registration_pending_) && state_ != WatchAncsLinkState::kFailed;
    host_ = true; interface_ = ESP_GATT_IF_NONE; registered_ = false;
    host_registration_pending_ = true;
    host_registration_started_ms_ = Now();
    Transition(enabled_ ? WatchAncsLinkState::kOffline : WatchAncsLinkState::kDisabled);
    if (!ConfigureSecurity() || esp_ble_gatt_set_local_mtu(512) != ESP_OK || esp_ble_gattc_register_callback(GattCallback) != ESP_OK ||
        esp_ble_gattc_app_register(kAppId) != ESP_OK) {
        host_registration_pending_ = false;
        Retire(WatchAncsLinkState::kFailed); return false;
    }
    return true;
}
void WatchAncsClient::ExpireHostRegistration() {
    if (!host_ || !host_registration_pending_) return;
    const auto now = Now();
    if (now >= host_registration_started_ms_ && now - host_registration_started_ms_ < kPhaseTimeoutMs) return;
    host_registration_pending_ = false;
    Retire(WatchAncsLinkState::kFailed);
}
void WatchAncsClient::HostStopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ControlRemovalFenced()) {
        control_removal_.phase = WatchControlRemovalPhase::kFailed;
        control_removal_.error = WatchControlRemovalError::kHostStopped;
    }
    Retire(WatchAncsLinkState::kOffline);
    host_ = host_registration_pending_ = false;
}
void WatchAncsClient::HostStopped() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ControlRemovalFenced()) {
        control_removal_.phase = WatchControlRemovalPhase::kFailed;
        control_removal_.error = WatchControlRemovalError::kHostStopped;
    }
    // A confirmed native host stop drains the old operation; HostStopping alone does not.
    control_removal_native_pending_ = false;
    ResetLink(); host_ = registered_ = connected_ = retiring_ = false;
    host_registration_pending_ = false;
    cleanup_failed_ = false;
    retirement_pending_ = false;
    device_pairing_window_ = false;
    interface_ = ESP_GATT_IF_NONE;
    registering_.reset();
    Transition(WatchAncsLinkState::kOffline);
}
bool WatchAncsClient::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retirement_pending_) Retire(WatchAncsLinkState::kDisabled);
    enabled_ = enabled; engine_.SetEnabled(enabled);
    if (device_pairing_window_ && DevicePairingWindowValid()) return true;
    if (control_peer_authorized_ || restoring_control_ || control_bond_check_) return true;
    if (!enabled) { Retire(WatchAncsLinkState::kDisabled); return true; }
    if (connected_ && !retiring_ && registered_ && state_ == WatchAncsLinkState::kDisabled)
        Transition(WatchAncsLinkState::kPeerConsent);
    return true;
}
void WatchAncsClient::DisableCollection() {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
    engine_.SetEnabled(false);
    early_notifications_.clear(); write_.reset(); session_ = 0;
    if (device_pairing_window_ || control_peer_authorized_ || restoring_control_ || control_bond_check_) return;
    retirement_pending_ = connected_;
    Transition(WatchAncsLinkState::kDisabled);
}
bool WatchAncsClient::Confirm(uint64_t confirmation, bool accept) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_pairing_peer_ || device_pairing_window_ || control_peer_authorized_ || restoring_control_ || !enabled_) return false;
    return ConfirmLocked(confirmation, accept);
}
bool WatchAncsClient::ConfirmDevicePairing(uint64_t confirmation, bool accept) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_pairing_ || !device_pairing_peer_ || !DevicePairingWindowValid()) return false;
    return ConfirmLocked(confirmation, accept);
}
bool WatchAncsClient::ConfirmControlPairing(uint64_t window, uint64_t confirmation, bool accept) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!window || window != device_pairing_generation_ || !control_pairing_ || !DevicePairingWindowValid()) return false;
    return ConfirmLocked(confirmation, accept);
}
bool WatchAncsClient::ConfirmLocked(uint64_t confirmation, bool accept) {
    if (!host_ || retiring_ || cleanup_failed_ || !connected_ || !confirmation || confirmation != confirmation_) return false;
    if (Now() - phase_started_ms_ >= kPhaseTimeoutMs) { Retire(WatchAncsLinkState::kFailed); return false; }
    if (state_ == WatchAncsLinkState::kPeerConsent) {
        if (!accept) { Retire(WatchAncsLinkState::kDisabled); return true; }
        control_bond_check_ = false;
        if (android_peer_ || device_pairing_peer_ || control_pairing_) {
            // The phone is the GATT client. No ANCS virtual connection or
            // duplicate MTU exchange is needed for watch-hosted services.
            Transition(WatchAncsLinkState::kPairing);
            if (esp_ble_set_encryption(peer_.data(), ESP_BLE_SEC_ENCRYPT_MITM) != ESP_OK) {
                Retire(WatchAncsLinkState::kFailed); return false;
            }
            return true;
        }
        Transition(WatchAncsLinkState::kOpening);
        if (esp_ble_gattc_open(interface_, peer_.data(), address_type_, true) != ESP_OK) {
            Retire(WatchAncsLinkState::kFailed); return false;
        }
        return true;
    }
    if (state_ == WatchAncsLinkState::kConfirmNumber) {
        Transition(WatchAncsLinkState::kPairing);
        const bool succeeded = esp_ble_confirm_reply(peer_.data(), accept) == ESP_OK;
        if (!succeeded || !accept) Retire(succeeded ? WatchAncsLinkState::kDisabled : WatchAncsLinkState::kFailed);
        return succeeded;
    }
    return false;
}

bool WatchAncsClient::GapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    if (!param) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    ExpirePhase();
    const bool pairing = host_ && (enabled_ || device_pairing_peer_ || control_pairing_ || restoring_control_) && !retiring_ &&
        (state_ == WatchAncsLinkState::kPairing || state_ == WatchAncsLinkState::kConfirmNumber);
    switch (event) {
        case ESP_GAP_BLE_SEC_REQ_EVT:
            (void)esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr,
                pairing && Peer(param->ble_security.ble_req.bd_addr));
            return true;
        case ESP_GAP_BLE_NC_REQ_EVT:
            if ((restoring_control_ || control_bond_check_) && Peer(param->ble_security.key_notif.bd_addr)) {
                (void)esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, false);
                Retire(WatchAncsLinkState::kFailed);
                return true;
            }
            if (pairing && Peer(param->ble_security.key_notif.bd_addr) && param->ble_security.key_notif.passkey <= 999999) {
                Transition(WatchAncsLinkState::kConfirmNumber);
                passkey_ = param->ble_security.key_notif.passkey;
            } else (void)esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, false);
            return true;
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            if (control_bond_check_ && Peer(param->ble_security.auth_cmpl.bd_addr)) {
                const auto& auth = param->ble_security.auth_cmpl;
                if (!auth.success || (auth.auth_mode & ESP_LE_AUTH_REQ_SC_MITM_BOND) != ESP_LE_AUTH_REQ_SC_MITM_BOND)
                    Retire(WatchAncsLinkState::kFailed);
                else security_ready_ = true;
                return true;
            }
            if (security_ready_ && Peer(param->ble_security.auth_cmpl.bd_addr) &&
                (!param->ble_security.auth_cmpl.success ||
                 (param->ble_security.auth_cmpl.auth_mode & ESP_LE_AUTH_REQ_SC_MITM_BOND) != ESP_LE_AUTH_REQ_SC_MITM_BOND)) {
                Retire(WatchAncsLinkState::kFailed); return true;
            }
            if (pairing && Peer(param->ble_security.auth_cmpl.bd_addr)) {
                const auto& auth = param->ble_security.auth_cmpl;
                if (state_ != WatchAncsLinkState::kPairing || !auth.success ||
                    (auth.auth_mode & ESP_LE_AUTH_REQ_SC_MITM_BOND) != ESP_LE_AUTH_REQ_SC_MITM_BOND) {
                    Retire(WatchAncsLinkState::kFailed);
                } else {
                    security_ready_ = true;
                    if (restoring_control_) return true;
                    if (control_pairing_) {
                        control_peer_authorized_ = true;
                        control_grant_window_ = device_pairing_generation_;
                        EndDevicePairingWindow(WatchAncsLinkState::kFailed);
                        return true;
                    }
                    Transition(device_pairing_peer_ ? WatchAncsLinkState::kDevicePairingReady :
                        android_peer_ ? WatchAncsLinkState::kAndroidAuthorizing : WatchAncsLinkState::kDiscovering);
                    if (!android_peer_ && !device_pairing_peer_ && esp_ble_gattc_search_service(interface_, connection_, nullptr) != ESP_OK)
                        Retire(WatchAncsLinkState::kFailed);
                }
            }
            return true;
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            (void)esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, false, 0);
            if (Peer(param->ble_security.ble_req.bd_addr)) Retire(WatchAncsLinkState::kFailed);
            return true;
        case ESP_GAP_BLE_OOB_REQ_EVT:
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            if (Peer(param->ble_security.ble_req.bd_addr)) Retire(WatchAncsLinkState::kFailed);
            return true;
        case ESP_GAP_BLE_KEY_EVT:
            if ((restoring_control_ || control_bond_check_ || security_ready_) &&
                Peer(param->ble_security.ble_key.bd_addr)) Retire(WatchAncsLinkState::kFailed);
            return true;
        case ESP_GAP_BLE_LOCAL_IR_EVT:
        case ESP_GAP_BLE_LOCAL_ER_EVT:
            return true;
        case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
            if (control_removal_native_pending_ &&
                std::equal(control_removal_native_address_.begin(), control_removal_native_address_.end(),
                           param->remove_bond_dev_cmpl.bd_addr)) {
                control_removal_native_pending_ = false;
                if (control_removal_.phase == WatchControlRemovalPhase::kRemovingNative) {
                    if (param->remove_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                        control_removal_.phase = WatchControlRemovalPhase::kVerifying;
                    } else {
                        control_removal_.phase = WatchControlRemovalPhase::kFailed;
                        control_removal_.error = WatchControlRemovalError::kNative;
                    }
                }
            }
            if (param->remove_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS && Peer(param->remove_bond_dev_cmpl.bd_addr)) {
                if (restoring_bond_) revoke_bond_ = restoring_bond_;
                Retire(WatchAncsLinkState::kDisabled);
            }
            return true;
        case ESP_GAP_BLE_CLEAR_BOND_DEV_COMPLETE_EVT:
            if (param->clear_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS) {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
                clear_control_bonds_ = true;
#endif
                if (connected_) Retire(WatchAncsLinkState::kDisabled);
            }
            return true;
        default: return false;
    }
}

bool WatchAncsClient::FindCharacteristic(uint16_t start, uint16_t end, esp_bt_uuid_t uuid, uint8_t properties, uint16_t& handle) {
    esp_gattc_char_elem_t values[2]{}; uint16_t count = 2;
    if (!start || end < start || esp_ble_gattc_get_char_by_uuid(interface_, connection_, start, end, uuid, values, &count) != ESP_GATT_OK ||
        count != 1 || (values[0].properties & properties) != properties || !values[0].char_handle) return false;
    handle = values[0].char_handle; return true;
}
void WatchAncsClient::Subscribe() {
    descriptor_ = 0;
    if (registering_) { Retire(WatchAncsLinkState::kFailed); return; }
    registering_ = NotifyRegistration{peer_, subscriptions_[subscribing_], link_generation_};
    if (esp_ble_gattc_register_for_notify(interface_, peer_.data(), subscriptions_[subscribing_]) != ESP_OK) {
        registering_.reset();
        Retire(WatchAncsLinkState::kFailed);
    }
}
void WatchAncsClient::Pump() {
    if (state_ != WatchAncsLinkState::kReady || retiring_) return;
    auto request = engine_.NextRequest(session_, Now());
    if (engine_.State() == WatchAncsState::kFailed) { Retire(WatchAncsLinkState::kFailed); return; }
    if (!request) return;
    if (write_ || request->bytes.size() > static_cast<size_t>(mtu_ - 3)) { Retire(WatchAncsLinkState::kFailed); return; }
    write_ = std::move(request);
    if (esp_ble_gattc_write_char(interface_, connection_, control_, static_cast<uint16_t>(write_->bytes.size()),
            write_->bytes.data(), ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM) != ESP_OK) {
        Retire(WatchAncsLinkState::kFailed);
    }
}
void WatchAncsClient::GattEvent(esp_gattc_cb_event_t event, esp_gatt_if_t interface, esp_ble_gattc_cb_param_t* param) {
    if (!param) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host_) return;
    if (event == ESP_GATTC_REG_EVT) {
        ExpireHostRegistration();
        if (param->reg.app_id != kAppId || !host_registration_pending_) return;
        host_registration_pending_ = false;
        if (param->reg.status != ESP_GATT_OK || interface == ESP_GATT_IF_NONE) {
            registered_ = false;
            Retire(WatchAncsLinkState::kFailed); return;
        }
        interface_ = interface; registered_ = true; return;
    }
    if (!registered_ || (interface != interface_ && interface != ESP_GATT_IF_NONE)) return;
    if (event == ESP_GATTC_UNREG_FOR_NOTIFY_EVT && param->unreg_for_notify.status != ESP_GATT_OK) {
        cleanup_failed_ = true; Retire(WatchAncsLinkState::kFailed); return;
    }
    if (event == ESP_GATTC_REG_FOR_NOTIFY_EVT) {
        if (!registering_ || param->reg_for_notify.handle != registering_->handle) return;
        const auto registration = *registering_; registering_.reset();
        const bool expected = connected_ && enabled_ && !retiring_ && registration.generation == link_generation_ &&
            state_ == WatchAncsLinkState::kSubscribing && !descriptor_ &&
            subscribing_ < subscriptions_.size() && registration.handle == subscriptions_[subscribing_];
        if (!expected) {
            if (param->reg_for_notify.status == ESP_GATT_OK) {
                auto address = registration.peer;
                if (esp_ble_gattc_unregister_for_notify(interface_, address.data(), registration.handle) != ESP_OK) {
                    cleanup_failed_ = true; Retire(WatchAncsLinkState::kFailed);
                }
            }
            return;
        }
        if (param->reg_for_notify.status == ESP_GATT_OK) registered_notifications_[subscribing_] = true;
    }
    if (event == ESP_GATTC_CONNECT_EVT) {
        if (connected_ || param->connect.link_role != 1) return;
        const bool control = control_pairing_;
        ResetLink(); control_pairing_ = control;
        connected_ = true; retiring_ = false; connection_ = param->connect.conn_id;
        if (++serial_ == 0) ++serial_;
        link_generation_ = serial_;
        address_type_ = param->connect.ble_addr_type;
        std::copy_n(param->connect.remote_bda, 6, peer_.begin());
        if (ControlRemovalFenced()) { Retire(WatchAncsLinkState::kDisabled); return; }
        DevicePairingWindowValid();
        if (!retiring_) Transition(control_pairing_ || (enabled_ && !device_pairing_window_)
            ? WatchAncsLinkState::kPeerConsent : WatchAncsLinkState::kDisabled);
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        control_bond_check_ = !retiring_ && !device_pairing_window_;
#endif
        return;
    }
    if (event == ESP_GATTC_DISCONNECT_EVT) {
        if (!Peer(param->disconnect.remote_bda) || param->disconnect.conn_id != connection_) return;
        device_pairing_window_ = false;
        ResetLink(); connected_ = retiring_ = false;
        Transition(enabled_ ? WatchAncsLinkState::kOffline : WatchAncsLinkState::kDisabled);
        return;
    }
    if (!connected_ || retiring_ || (!enabled_ && !device_pairing_peer_ && !control_pairing_ && !control_peer_authorized_ && !restoring_control_)) return;
    if (ExpirePhase()) return;
    if (device_pairing_peer_ || control_pairing_ || control_peer_authorized_ || restoring_control_) {
        if (event == ESP_GATTC_CLOSE_EVT && Peer(param->close.remote_bda) && param->close.conn_id == connection_)
            Retire(WatchAncsLinkState::kFailed);
        return;
    }
    switch (event) {
        case ESP_GATTC_OPEN_EVT:
            if (state_ != WatchAncsLinkState::kOpening || !Peer(param->open.remote_bda)) return;
            if (param->open.status != ESP_GATT_OK) { Retire(WatchAncsLinkState::kFailed); return; }
            connection_ = param->open.conn_id; Transition(WatchAncsLinkState::kNegotiating);
            if (esp_ble_gattc_send_mtu_req(interface_, connection_) != ESP_OK) Retire(WatchAncsLinkState::kFailed);
            break;
        case ESP_GATTC_CFG_MTU_EVT:
            if (state_ != WatchAncsLinkState::kNegotiating || param->cfg_mtu.conn_id != connection_) return;
            if (param->cfg_mtu.status != ESP_GATT_OK || param->cfg_mtu.mtu < 23 || param->cfg_mtu.mtu > 517) {
                Retire(WatchAncsLinkState::kFailed); return;
            }
            mtu_ = param->cfg_mtu.mtu; Transition(WatchAncsLinkState::kPairing);
            if (esp_ble_set_encryption(peer_.data(), ESP_BLE_SEC_ENCRYPT_MITM) != ESP_OK) Retire(WatchAncsLinkState::kFailed);
            break;
        case ESP_GATTC_SEARCH_RES_EVT: {
            if (state_ != WatchAncsLinkState::kDiscovering || param->search_res.conn_id != connection_) return;
            const auto& found = param->search_res;
            if (IsUuid(found.srvc_id.uuid, kAncs)) {
                if (ancs_start_) { Retire(WatchAncsLinkState::kFailed); return; }
                ancs_start_ = found.start_handle; ancs_end_ = found.end_handle;
            } else if (found.srvc_id.uuid.len == ESP_UUID_LEN_16 && found.srvc_id.uuid.uuid.uuid16 == 0x1801) {
                gatt_start_ = found.start_handle; gatt_end_ = found.end_handle;
            }
            break;
        }
        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (state_ != WatchAncsLinkState::kDiscovering || param->search_cmpl.conn_id != connection_) return;
            if (param->search_cmpl.status != ESP_GATT_OK || !security_ready_ ||
                !FindCharacteristic(gatt_start_, gatt_end_, Uuid(0x2a05), ESP_GATT_CHAR_PROP_BIT_INDICATE, subscriptions_[0])) {
                Retire(WatchAncsLinkState::kFailed); return;
            }
            if (ancs_start_ && (!FindCharacteristic(ancs_start_, ancs_end_, Uuid(kData), ESP_GATT_CHAR_PROP_BIT_NOTIFY, subscriptions_[1]) ||
                !FindCharacteristic(ancs_start_, ancs_end_, Uuid(kSource), ESP_GATT_CHAR_PROP_BIT_NOTIFY, subscriptions_[2]) ||
                !FindCharacteristic(ancs_start_, ancs_end_, Uuid(kControl), ESP_GATT_CHAR_PROP_BIT_WRITE, control_))) {
                Retire(WatchAncsLinkState::kUnavailable); return;
            }
            Transition(WatchAncsLinkState::kSubscribing); Subscribe(); break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
            if (state_ != WatchAncsLinkState::kSubscribing || descriptor_ || param->reg_for_notify.handle != subscriptions_[subscribing_]) return;
            esp_gattc_descr_elem_t values[2]{}; uint16_t count = 2;
            if (param->reg_for_notify.status != ESP_GATT_OK ||
                esp_ble_gattc_get_descr_by_char_handle(interface_, connection_, subscriptions_[subscribing_], Uuid(0x2902), values, &count) != ESP_GATT_OK ||
                count != 1 || !values[0].handle) { Retire(WatchAncsLinkState::kFailed); return; }
            descriptor_ = values[0].handle;
            if (subscribing_ == 2) session_ = engine_.BeginSession();
            uint8_t value[]{static_cast<uint8_t>(subscribing_ == 0 ? 2 : 1), 0};
            if (esp_ble_gattc_write_char_descr(interface_, connection_, descriptor_, sizeof(value), value,
                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM) != ESP_OK) Retire(WatchAncsLinkState::kFailed);
            break;
        }
        case ESP_GATTC_WRITE_DESCR_EVT:
            if (state_ != WatchAncsLinkState::kSubscribing || !descriptor_ || param->write.conn_id != connection_ || param->write.handle != descriptor_) return;
            if (param->write.status != ESP_GATT_OK) { Retire(WatchAncsLinkState::kFailed); return; }
            if (subscribing_ == 0 && !ancs_start_) { Transition(WatchAncsLinkState::kUnavailable); return; }
            if (++subscribing_ < subscriptions_.size()) { Subscribe(); return; }
            if (!engine_.Authorized(session_)) { Retire(WatchAncsLinkState::kFailed); return; }
            Transition(WatchAncsLinkState::kReady);
            for (const auto& bytes : early_notifications_) {
                if (!engine_.NotificationSource(session_, bytes.data(), bytes.size())) { Retire(WatchAncsLinkState::kFailed); return; }
            }
            early_notifications_.clear(); Pump(); break;
        case ESP_GATTC_NOTIFY_EVT: {
            const auto& value = param->notify;
            if (value.conn_id != connection_ || !Peer(value.remote_bda)) return;
            if (value.handle == subscriptions_[0]) { ServiceChanged(); return; }
            if (value.handle == subscriptions_[2] && state_ == WatchAncsLinkState::kSubscribing && subscribing_ == 2 && descriptor_) {
                if (!value.is_notify || value.value_len != 8 || !value.value || early_notifications_.size() >= 20) {
                    Retire(WatchAncsLinkState::kFailed); return;
                }
                std::array<uint8_t, 8> bytes{}; std::copy_n(value.value, 8, bytes.begin()); early_notifications_.push_back(bytes); return;
            }
            if (state_ != WatchAncsLinkState::kReady) return;
            if (!value.is_notify || (value.handle == subscriptions_[2]
                    ? !engine_.NotificationSource(session_, value.value, value.value_len)
                    : value.handle == subscriptions_[1] && !engine_.DataSource(session_, value.value, value.value_len, Now()))) {
                Retire(WatchAncsLinkState::kFailed); return;
            }
            Pump(); break;
        }
        case ESP_GATTC_WRITE_CHAR_EVT: {
            if (state_ != WatchAncsLinkState::kReady || !write_ || param->write.conn_id != connection_ || param->write.handle != control_) return;
            const auto status = param->write.status;
            const auto result = status == ESP_GATT_OK ? WatchAncsWriteResult::kAccepted :
                status >= 0xa0 && status <= 0xa3 ? WatchAncsWriteResult::kRejected : WatchAncsWriteResult::kTransportFailure;
            const auto request = write_->request; write_.reset();
            if (!engine_.RequestWritten(session_, request, result, Now())) { Retire(WatchAncsLinkState::kFailed); return; }
            Pump(); break;
        }
        case ESP_GATTC_SRVC_CHG_EVT:
            if (Peer(param->srvc_chg.remote_bda)) ServiceChanged();
            break;
        case ESP_GATTC_CLOSE_EVT:
            if (Peer(param->close.remote_bda) && param->close.conn_id == connection_) Retire(WatchAncsLinkState::kFailed);
            break;
        default: break;
    }
}
void WatchAncsClient::Poll() {
    PollControlRemoval();
    PollControlBond();
    std::lock_guard<std::mutex> lock(mutex_);
    ExpireHostRegistration();
    if (retirement_pending_) Retire(WatchAncsLinkState::kDisabled);
    if (device_pairing_window_ && !DevicePairingWindowValid()) return;
    if (!host_ || (!enabled_ && !device_pairing_peer_ && !control_pairing_ && !restoring_control_) || retiring_) return;
    if (state_ == WatchAncsLinkState::kReady) { Pump(); return; }
    ExpirePhase();
}
bool WatchAncsClient::MarkRead(uint64_t session, uint32_t uid, uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutex_); return engine_.MarkRead(session, uid, revision);
}
bool WatchAncsClient::RequestAppName(uint64_t session, uint32_t uid, uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto snapshot = engine_.Snapshot();
    const auto item = std::find_if(snapshot.notifications.begin(), snapshot.notifications.end(), [=](const auto& value) {
        return value.uid == uid && value.revision == revision;
    });
    // Do not silently truncate a Control Point command at a small negotiated MTU.
    if (item == snapshot.notifications.end() || item->app_identifier.size() + 3 > static_cast<size_t>(mtu_ - 3)) return false;
    const bool result = engine_.RequestAppName(session, uid, revision); Pump(); return result;
}
bool WatchAncsClient::Clear(uint64_t session) {
    std::lock_guard<std::mutex> lock(mutex_); return engine_.Clear(session);
}
WatchAncsClientSnapshot WatchAncsClient::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {enabled_, state_, confirmation_, passkey_, engine_.Snapshot(), android_peer_, link_generation_, device_pairing_peer_, control_pairing_};
}
bool WatchAncsClient::SelectAndroidPeer(uint16_t connection, const uint8_t* address) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host_ || !registered_ || !enabled_ || retiring_ || cleanup_failed_ ||
        !Peer(address) || connection != connection_ || state_ != WatchAncsLinkState::kPeerConsent ||
        android_peer_ || device_pairing_window_ || device_pairing_peer_ || ExpirePhase()) return false;
    android_peer_ = true;
    // Invalidate an already rendered iPhone-specific permission prompt.
    Transition(WatchAncsLinkState::kPeerConsent);
    return true;
}
uint64_t WatchAncsClient::AndroidPeerLease(uint16_t connection, const uint8_t* address) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host_ || !enabled_ || retiring_ || cleanup_failed_ || !android_peer_ || !security_ready_ ||
        connection != connection_ || !Peer(address) || state_ != WatchAncsLinkState::kAndroidAuthorizing || ExpirePhase()) return 0;
    return link_generation_;
}
bool WatchAncsClient::RenewAndroidPeer(uint64_t lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lease || lease != link_generation_ || !host_ || !enabled_ || retiring_ || cleanup_failed_ ||
        !android_peer_ || !security_ready_ || state_ != WatchAncsLinkState::kAndroidAuthorizing || ExpirePhase()) return false;
    phase_started_ms_ = Now(); return true;
}

uint64_t WatchAncsClient::OpenDevicePairingWindow() {
    std::lock_guard<std::mutex> lock(mutex_);
    return OpenPairingWindow(false);
}
uint64_t WatchAncsClient::OpenControlPairingWindow() {
    std::lock_guard<std::mutex> lock(mutex_);
    return OpenPairingWindow(true);
}
bool WatchAncsClient::CanOpenPairingWindow(bool control_only) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PairingWindowAvailable(control_only);
}
bool WatchAncsClient::PairingWindowAvailable(bool control) const {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (control_bond_busy_) return false;
#endif
    if (ControlRemovalFenced() || !host_ || !registered_ || host_registration_pending_ || retiring_ || cleanup_failed_ || retirement_pending_ ||
        (control && control_peer_authorized_) ||
        device_pairing_window_ || android_peer_ || device_pairing_peer_ || revoke_bond_ || clear_control_bonds_ ||
        device_pairing_generation_ == std::numeric_limits<uint64_t>::max() ||
        (state_ != WatchAncsLinkState::kControlReady &&
         state_ != WatchAncsLinkState::kOffline && state_ != WatchAncsLinkState::kDisabled &&
         state_ != WatchAncsLinkState::kPeerConsent)) return false;
    return true;
}
uint64_t WatchAncsClient::OpenPairingWindow(bool control) {
    ExpirePhase();
    if (!PairingWindowAvailable(control)) return 0;
    device_pairing_started_ms_ = Now();
    device_pairing_window_ = true;
    control_pairing_ = control;
    control_bond_check_ = false;
    // Invalidate any rendered notification consent before selecting a purpose.
    Transition(control && connected_ ? WatchAncsLinkState::kPeerConsent : WatchAncsLinkState::kDisabled);
    return ++device_pairing_generation_;
}
void WatchAncsClient::CloseDevicePairingWindow(uint64_t window) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window && window == control_grant_window_ && control_bond_ready_) provisional_bond_.reset();
    if (!window || window != device_pairing_generation_ || !device_pairing_window_) return;
    EndDevicePairingWindow(WatchAncsLinkState::kDisabled);
}
bool WatchAncsClient::DevicePairingWindowCurrent(uint64_t window) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !control_pairing_ && window && window == device_pairing_generation_ && host_ && registered_ &&
        !retiring_ && !cleanup_failed_ && !retirement_pending_ && DevicePairingWindowValid();
}
bool WatchAncsClient::ControlPairingWindowCurrent(uint64_t window) {
    std::lock_guard<std::mutex> lock(mutex_);
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (window && window == control_grant_window_ && window == device_pairing_generation_ &&
        control_peer_authorized_ && security_ready_ && !control_bond_ready_ &&
        host_ && registered_ && connected_ && !retiring_ && !cleanup_failed_ && !retirement_pending_) {
        const auto now = Now();
        return now >= device_pairing_started_ms_ && now - device_pairing_started_ms_ < kDevicePairingWindowMs;
    }
#endif
    return control_pairing_ && window && window == device_pairing_generation_ && host_ && registered_ &&
        !retiring_ && !cleanup_failed_ && !retirement_pending_ && DevicePairingWindowValid();
}
uint64_t WatchAncsClient::ControlPairingLease(uint64_t window) const {
    std::lock_guard<std::mutex> lock(mutex_);
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (!control_bond_ready_) return 0;
#endif
    return window && window == control_grant_window_ && window == device_pairing_generation_ &&
        host_ && registered_ && connected_ && !retiring_ && !cleanup_failed_ && !retirement_pending_ &&
        control_peer_authorized_ && security_ready_ && state_ == WatchAncsLinkState::kControlReady
        ? link_generation_ : 0;
}
void WatchAncsClient::CancelControlPairing(uint64_t window) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!window || window != device_pairing_generation_) return;
        if (control_pairing_ || window == control_grant_window_) {
            if (provisional_bond_) revoke_bond_ = provisional_bond_;
            Retire(WatchAncsLinkState::kDisabled);
        }
    }
    PollControlBond();
}
bool WatchAncsClient::SelectDevicePairingPeer(uint16_t connection, const uint8_t* address) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host_ || !registered_ || retiring_ || cleanup_failed_ || retirement_pending_ ||
        control_pairing_ || !DevicePairingWindowValid() || android_peer_ || device_pairing_peer_ ||
        state_ != WatchAncsLinkState::kDisabled || connection != connection_ || !Peer(address)) return false;
    device_pairing_peer_ = true;
    Transition(control_peer_authorized_ && security_ready_ ? WatchAncsLinkState::kDevicePairingReady :
        WatchAncsLinkState::kPeerConsent);
    return true;
}
uint64_t WatchAncsClient::DevicePairingPeerLease(uint16_t connection, const uint8_t* address) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host_ || !registered_ || retiring_ || cleanup_failed_ || retirement_pending_ ||
        !DevicePairingWindowValid() || !device_pairing_peer_ || !security_ready_ ||
        state_ != WatchAncsLinkState::kDevicePairingReady || connection != connection_ || !Peer(address)) return 0;
    return link_generation_;
}
bool WatchAncsClient::DevicePairingCommitAllowed(uint64_t window, uint64_t lease, uint16_t connection,
                                               const uint8_t* address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Now();
    return window && window == device_pairing_generation_ && device_pairing_window_ &&
        host_ && registered_ && !retiring_ && !cleanup_failed_ && !retirement_pending_ &&
        now >= device_pairing_started_ms_ && now - device_pairing_started_ms_ < kDevicePairingWindowMs &&
        lease && lease == link_generation_ && device_pairing_peer_ && security_ready_ &&
        state_ == WatchAncsLinkState::kDevicePairingReady && connection == connection_ && Peer(address);
}

bool WatchAncsClient::AuthorizeControlPeer(uint64_t window, uint64_t lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!window || window != device_pairing_generation_ || !lease || lease != link_generation_ ||
        !host_ || !registered_ || !connected_ || retiring_ || cleanup_failed_ || retirement_pending_ ||
        !DevicePairingWindowValid() || !device_pairing_peer_ || !security_ready_ ||
        state_ != WatchAncsLinkState::kDevicePairingReady) return false;
    control_peer_authorized_ = true;
    control_grant_window_ = window;
    return true;
}

uint64_t WatchAncsClient::ControlPeerLease(uint16_t connection, const uint8_t* address) const {
    std::lock_guard<std::mutex> lock(mutex_);
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (!control_bond_ready_) return 0;
#endif
    if (!host_ || !registered_ || retiring_ || cleanup_failed_ || retirement_pending_ ||
        !control_peer_authorized_ || !security_ready_ || connection != connection_ || !Peer(address)) return 0;
    return link_generation_;
}

bool WatchAncsClient::ControlRemovalFenced() const {
    using Phase = WatchControlRemovalPhase;
    return control_removal_busy_ || control_removal_native_pending_ || control_removal_.phase == Phase::kRevoking ||
        control_removal_.phase == Phase::kRemovingNative || control_removal_.phase == Phase::kVerifying;
}
void WatchAncsClient::InvalidateSavedControlPeers() {
    saved_control_peers_.clear();
    if (saved_control_revision_ != UINT64_MAX) ++saved_control_revision_;
}
bool WatchAncsClient::CanInspectSavedControlPeersLocked() const {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    return host_ && registered_ && !connected_ && !enabled_ && !retiring_ && !cleanup_failed_ &&
        !retirement_pending_ && !device_pairing_window_ && !control_bond_busy_ && !ControlRemovalFenced() &&
        !revoke_bond_ && !clear_control_bonds_ && !provisional_bond_ && saved_control_revision_ != UINT64_MAX;
#else
    return false;
#endif
}
bool WatchAncsClient::CanInspectSavedControlPeers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CanInspectSavedControlPeersLocked();
}
bool WatchAncsClient::InspectSavedControlPeers(std::vector<WatchSavedControlPeer>& peers) {
    peers.clear();
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!CanInspectSavedControlPeersLocked()) return false;
        InvalidateSavedControlPeers();
        revision = saved_control_revision_;
        control_bond_busy_ = true;
    }
    std::vector<watch_control::StoredControlBond> bonds;
    const auto result = watch_control::ListApprovedBonds(bonds);
    std::lock_guard<std::mutex> lock(mutex_);
    control_bond_busy_ = false;
    if (saved_control_revision_ != revision || !CanInspectSavedControlPeersLocked() ||
        (result != watch_control::BondStatus::kFound && result != watch_control::BondStatus::kAbsent) ||
        bonds.size() > UINT64_MAX - serial_) return false;
    saved_control_inspected_ms_ = Now();
    for (const auto& bond : bonds) {
        WatchSavedControlPeer peer{++serial_, bond.address};
        saved_control_peers_.push_back({peer, bond});
        peers.push_back(peer);
    }
    return true;
#else
    return false;
#endif
}
bool WatchAncsClient::CanForgetSavedControlPeerLocked(uint64_t selection) const {
    const auto now = Now();
    return selection && CanInspectSavedControlPeersLocked() && now >= saved_control_inspected_ms_ &&
        now - saved_control_inspected_ms_ < kDevicePairingWindowMs &&
        std::any_of(saved_control_peers_.begin(), saved_control_peers_.end(), [&](const auto& item) {
            return item.peer.selection == selection;
        });
}
bool WatchAncsClient::CanForgetSavedControlPeer(uint64_t selection) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CanForgetSavedControlPeerLocked(selection);
}
bool WatchAncsClient::ForgetSavedControlPeer(uint64_t selection) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CanForgetSavedControlPeerLocked(selection)) return false;
    const auto item = *std::find_if(saved_control_peers_.begin(), saved_control_peers_.end(), [&](const auto& value) {
        return value.peer.selection == selection;
    });
    control_removal_ = {selection, WatchControlRemovalPhase::kRevoking, WatchControlRemovalError::kNone};
    control_removal_fingerprint_ = item.bond.fingerprint;
    control_removal_peer_ = item.bond.address;
    control_removal_native_address_ = {};
    control_removal_started_ms_ = Now();
    revoke_bond_ = item.bond.fingerprint;
    Retire(WatchAncsLinkState::kDisabled);
    return true;
}
bool WatchAncsClient::CanForgetControlPeerLocked(uint64_t generation) const {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    return generation && generation == link_generation_ && !control_bond_busy_ && !ControlRemovalFenced() &&
        host_ && registered_ && connected_ && !retiring_ && !cleanup_failed_ && !retirement_pending_ &&
        !device_pairing_window_ && !revoke_bond_ && !clear_control_bonds_ && !provisional_bond_ &&
        control_peer_authorized_ && control_bond_ready_ && security_ready_ && restoring_bond_.has_value() &&
        state_ == WatchAncsLinkState::kControlReady;
#else
    (void)generation;
    return false;
#endif
}
bool WatchAncsClient::CanForgetControlPeer(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CanForgetControlPeerLocked(generation);
}
bool WatchAncsClient::ForgetControlPeer(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CanForgetControlPeerLocked(generation)) return false;
    control_removal_ = {generation, WatchControlRemovalPhase::kRevoking, WatchControlRemovalError::kNone};
    control_removal_fingerprint_ = *restoring_bond_;
    revoke_bond_ = restoring_bond_;
    control_removal_peer_ = peer_;
    control_removal_native_address_ = {};
    control_removal_started_ms_ = Now();
    // Revoke all in-memory tickets before any potentially blocking NVS/SDK I/O.
    Retire(WatchAncsLinkState::kDisabled);
    return true;
}
WatchControlRemovalSnapshot WatchAncsClient::ControlRemovalSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = control_removal_;
    result.io_pending = control_removal_busy_ ||
        (revoke_bond_ && *revoke_bond_ == control_removal_fingerprint_);
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    result.io_pending = result.io_pending || control_bond_busy_;
#endif
    return result;
}
void WatchAncsClient::PollControlRemoval() {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    constexpr uint64_t kControlRemovalTimeoutMs = 10000;
    using Phase = WatchControlRemovalPhase;
    using Error = WatchControlRemovalError;
    using namespace watch_control;
    WatchControlRemovalSnapshot operation;
    BondFingerprint expected{};
    std::array<uint8_t, 6> address{}, native_address{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (control_bond_busy_) return;
        const auto now = Now();
        if (control_removal_.phase == Phase::kRemovingNative) {
            if (now < control_removal_started_ms_ || now - control_removal_started_ms_ >= kControlRemovalTimeoutMs) {
                control_removal_.phase = Phase::kFailed;
                control_removal_.error = Error::kTimeout;
                // Keep the uncorrelated SDK request fenced until its callback or full host stop.
            }
            return;
        }
        if (control_removal_.phase != Phase::kRevoking && control_removal_.phase != Phase::kVerifying) return;
        operation = control_removal_;
        expected = control_removal_fingerprint_;
        address = control_removal_peer_;
        native_address = control_removal_native_address_;
        control_bond_busy_ = true;
        control_removal_busy_ = true;
    }
    const auto revoked = operation.phase == Phase::kRevoking ? Revoke(expected) : BondStatus::kRemoved;
    BondFingerprint found{};
    std::array<uint8_t, 6> stored_address{};
    auto status = revoked == BondStatus::kRemoved
        ? FindNativeBond(address.data(), found, stored_address) : revoked;
    if (operation.phase == Phase::kVerifying && status == BondStatus::kAbsent && native_address != address) {
        status = FindNativeBond(native_address.data(), found, stored_address);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    control_bond_busy_ = false;
    control_removal_busy_ = false;
    if (revoked == BondStatus::kRemoved && revoke_bond_ == expected) revoke_bond_.reset();
    if (control_removal_.peer_generation != operation.peer_generation || control_removal_.phase != operation.phase) return;
    const auto fail = [&](Error error) {
        control_removal_.phase = Phase::kFailed;
        control_removal_.error = error;
    };
    if (revoked != BondStatus::kRemoved) {
        cleanup_failed_ = true;
        Retire(WatchAncsLinkState::kFailed);
        fail(Error::kStorage);
    } else if (!host_ || !registered_) {
        fail(Error::kHostStopped);
    } else if (status == BondStatus::kAbsent) {
        control_removal_.phase = Phase::kComplete;
    } else if (status != BondStatus::kFound || found != expected) {
        fail(Error::kChangedBond);
    } else if (operation.phase == Phase::kVerifying) {
        fail(Error::kNative);
    } else {
        control_removal_native_address_ = stored_address;
        control_removal_started_ms_ = Now();
        control_removal_native_pending_ = true;
        control_removal_.phase = Phase::kRemovingNative;
        if (esp_ble_remove_bond_device(control_removal_native_address_.data()) != ESP_OK) {
            control_removal_native_pending_ = false;
            fail(Error::kNative);
        }
    }
#endif
}

void WatchAncsClient::PollControlBond() {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    using namespace watch_control;
    enum class Work { kClear, kRevoke, kCheck, kSave, kRestore };
    Work work;
    std::array<uint8_t, 6> address{};
    uint64_t generation = 0, window = 0;
    std::optional<BondFingerprint> expected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (control_bond_busy_ || ControlRemovalFenced()) return;
        if (clear_control_bonds_) work = Work::kClear;
        else if (revoke_bond_) { work = Work::kRevoke; expected = revoke_bond_; }
        else {
            if (!host_ || !registered_ || !connected_ || retiring_ || cleanup_failed_ || retirement_pending_) return;
            if (control_peer_authorized_ && !control_bond_ready_ && control_grant_window_) work = Work::kSave;
            else if (control_bond_check_) work = Work::kCheck;
            else if (restoring_control_ && security_ready_) { work = Work::kRestore; expected = restoring_bond_; }
            else return;
            address = peer_; generation = link_generation_; window = control_grant_window_;
        }
        control_bond_busy_ = true;
    }
    // SDK/NVS I/O runs outside the native owner lock. Every result is tied to
    // its original link, and only one storage operation can be in flight.
    BondFingerprint fingerprint{};
    BondStatus status = BondStatus::kAbsent;
    bool newly_approved = false;
    bool saved = false;
    if (work == Work::kClear) status = RevokeAll();
    else if (work == Work::kRevoke) status = Revoke(*expected);
    else {
        status = FindNativeBond(address.data(), fingerprint);
        if (status == BondStatus::kFound) {
            status = ReadApproval(fingerprint);
            if (work == Work::kSave) {
                newly_approved = status != BondStatus::kApproved;
                status = Approve(fingerprint, [&] {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto now = Now();
                    return host_ && connected_ && !retiring_ && !cleanup_failed_ && !retirement_pending_ &&
                        link_generation_ == generation && Peer(address.data()) && security_ready_ &&
                        control_peer_authorized_ && control_grant_window_ == window &&
                        now >= device_pairing_started_ms_ && now - device_pairing_started_ms_ < kDevicePairingWindowMs;
                });
                saved = status == BondStatus::kApproved;
                if (saved) {
                    BondFingerprint confirmed{};
                    if (FindNativeBond(address.data(), confirmed) != BondStatus::kFound || confirmed != fingerprint)
                        status = BondStatus::kInvalidBond;
                }
            }
        }
    }
    bool drain_revoke = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool current = host_ && registered_ && connected_ && !retiring_ && !cleanup_failed_ &&
            !retirement_pending_ && link_generation_ == generation && Peer(address.data());
        if (work == Work::kClear) {
            clear_control_bonds_ = false;
            revoke_bond_.reset();
            if (status != BondStatus::kRemoved) { cleanup_failed_ = true; Retire(WatchAncsLinkState::kFailed); }
        } else if (work == Work::kRevoke) {
            if (revoke_bond_ == expected) revoke_bond_.reset();
            if (status != BondStatus::kRemoved) { cleanup_failed_ = true; Retire(WatchAncsLinkState::kFailed); }
        } else if (!current || (work == Work::kSave &&
                       (!control_peer_authorized_ || control_grant_window_ != window ||
                        Now() < device_pairing_started_ms_ || Now() - device_pairing_started_ms_ >= kDevicePairingWindowMs))) {
            if (work == Work::kSave && newly_approved && saved) {
                revoke_bond_ = fingerprint;
                drain_revoke = true;
            }
            if (current && work == Work::kSave && control_grant_window_ == window)
                Retire(WatchAncsLinkState::kFailed);
        } else if (work == Work::kSave) {
            if (status == BondStatus::kApproved) {
                control_bond_ready_ = true;
                restoring_bond_ = fingerprint;
                if (newly_approved) provisional_bond_ = fingerprint;
            } else {
                if (newly_approved && saved) { revoke_bond_ = fingerprint; drain_revoke = true; }
                Retire(WatchAncsLinkState::kFailed);
            }
        } else if (work == Work::kCheck && control_bond_check_ && !device_pairing_window_) {
            control_bond_check_ = false;
            if (status == BondStatus::kApproved) {
                restoring_bond_ = fingerprint;
                restoring_control_ = true;
                Transition(WatchAncsLinkState::kPairing);
                if (!security_ready_ && esp_ble_set_encryption(peer_.data(), ESP_BLE_SEC_ENCRYPT_MITM) != ESP_OK)
                    Retire(WatchAncsLinkState::kFailed);
            } else if (status != BondStatus::kAbsent && status != BondStatus::kRemoved) Retire(WatchAncsLinkState::kFailed);
        } else if (work == Work::kRestore && restoring_control_) {
            if (security_ready_ && expected && fingerprint == *expected && status == BondStatus::kApproved) {
                restoring_control_ = false;
                control_peer_authorized_ = control_bond_ready_ = true;
                Transition(WatchAncsLinkState::kControlReady);
            } else Retire(WatchAncsLinkState::kFailed);
        }
        control_bond_busy_ = false;
    }
    if (drain_revoke) PollControlBond();
#endif
}
