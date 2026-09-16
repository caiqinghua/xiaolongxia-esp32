#include "watch/watch_ancs_client.h"
#include "watch/watch_blufi_security.h"
#include "boards/common/blufi_control_session.h"
#include "watch/watch_android_gatt_server.h"
#include "watch/watch_pairing_gatt_server.h"
#include "watch/watch_gatt_dispatcher.h"
#include "watch/watch_notification_owner_grant.h"
#include "watch/watch_voice_binding.h"
#include "esp_random.h"
#include "esp_gatt_common_api.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
#include "control_bond_fixture.h"
#endif

namespace {
using Bytes = std::vector<uint8_t>;
esp_gattc_cb_t callback = nullptr;
esp_gatts_cb_t server_callback = nullptr;
int64_t clock_us = 100000;
constexpr esp_gatt_if_t kInterface = 7;
uint16_t connection = 1;
uint8_t peer[]{1, 2, 3, 4, 5, 6};
std::vector<std::string> calls;
std::vector<std::array<uint8_t, 6>> removed_bond_addresses;
std::vector<uint16_t> registrations;
std::vector<uint16_t> unregistrations;
std::vector<std::pair<uint16_t, Bytes>> descriptor_writes;
std::vector<Bytes> control_writes;
std::vector<bool> confirmations, security_replies;
std::string fail_api;
bool missing_control = false;
constexpr esp_gatt_if_t kServerInterface = 9;
constexpr esp_gatt_if_t kPairingInterface = 10;
std::vector<std::pair<esp_gatt_status_t, Bytes>> pairing_responses;
std::array<uint8_t, 16> pairing_uuid{};
WatchPairingGattServer& PairingServer() { return WatchPairingGattServer::Get(); }
std::vector<std::pair<esp_gatt_status_t, Bytes>> server_responses;
std::vector<Bytes> server_indications;
std::array<uint8_t, 16> server_uuid{};
uint8_t random_calls = 0;
uint64_t binding_generation = 1;
uint64_t device_pairing_window = 0;
std::string bound_device = "11111111-1111-4111-8111-111111111111";
std::string bound_credential = "adv_" + std::string(43, 'x');
WatchAndroidGattServer& Server() { return WatchAndroidGattServer::Get(); }
bool ApplyOwner(const WatchAndroidGattServer::OwnerRequest& request) {
    return Server().ApplyOwnerAuthorization(request, binding_generation, 300, {});
}
esp_err_t Call(const char* name) {
    calls.emplace_back(name);
    return fail_api == name ? ESP_FAIL : ESP_OK;
}
WatchAncsClient& Client() { return WatchAncsClient::Get(); }
void Event(esp_gattc_cb_event_t event, esp_ble_gattc_cb_param_t& value) { assert(callback); callback(event, kInterface, &value); }
void Connected() {
    esp_ble_gattc_cb_param_t value{};
    value.connect.conn_id = connection; value.connect.link_role = 1;
    value.connect.ble_addr_type = BLE_ADDR_TYPE_RANDOM;
    std::memcpy(value.connect.remote_bda, peer, 6);
    Event(ESP_GATTC_CONNECT_EVT, value);
}
void Disconnected() {
    esp_ble_gattc_cb_param_t value{}; value.disconnect.conn_id = connection;
    std::memcpy(value.disconnect.remote_bda, peer, 6); Event(ESP_GATTC_DISCONNECT_EVT, value);
}
void Reset(bool enabled = true) {
    PairingServer().HostStopping(); PairingServer().HostStopped();
    Server().HostStopping(); Server().HostStopped(); Server().SetEnabled(false);
    Client().HostStopping(); Client().HostStopped(); Client().SetEnabled(false);
    calls.clear(); registrations.clear(); unregistrations.clear(); descriptor_writes.clear(); control_writes.clear();
    confirmations.clear(); security_replies.clear(); fail_api.clear(); missing_control = false; clock_us = 100000;
    server_responses.clear(); server_indications.clear();
    pairing_responses.clear();
    binding_generation = 1;
    ++connection;
    assert(Client().HostStarted());
    esp_ble_gattc_cb_param_t reg{}; reg.reg.app_id = 0x2da1; reg.reg.status = ESP_GATT_OK; Event(ESP_GATTC_REG_EVT, reg);
    assert(Client().SetEnabled(enabled)); Connected();
}
void Open() {
    const auto state = Client().Snapshot(); assert(state.state == WatchAncsLinkState::kPeerConsent && state.confirmation);
    assert(Client().NeedsResponsiveLink());
    assert(!Client().Confirm(state.confirmation + 1, true));
    assert(Client().Confirm(state.confirmation, true));
    assert(Client().Snapshot().state == WatchAncsLinkState::kOpening);
    esp_ble_gattc_cb_param_t value{}; value.open.status = ESP_GATT_OK; value.open.conn_id = connection;
    std::memcpy(value.open.remote_bda, peer, 6); Event(ESP_GATTC_OPEN_EVT, value);
    assert(Client().Snapshot().state == WatchAncsLinkState::kNegotiating);
    value = {}; value.cfg_mtu.conn_id = connection; value.cfg_mtu.status = ESP_GATT_OK; value.cfg_mtu.mtu = 512;
    Event(ESP_GATTC_CFG_MTU_EVT, value);
    assert(Client().Snapshot().state == WatchAncsLinkState::kPairing);
}
void Number(bool accept = true) {
    esp_ble_gap_cb_param_t gap{};
    std::memcpy(gap.ble_security.key_notif.bd_addr, peer, 6); gap.ble_security.key_notif.passkey = 123456;
    assert(Client().GapEvent(ESP_GAP_BLE_NC_REQ_EVT, &gap));
    assert(confirmations.empty());
    const auto snapshot = Client().Snapshot();
    assert(snapshot.state == WatchAncsLinkState::kConfirmNumber && snapshot.passkey == 123456);
    assert(Client().Confirm(snapshot.confirmation, accept));
    assert(confirmations == std::vector<bool>{accept});
}
void Auth(bool success = true, esp_ble_auth_req_t mode = ESP_LE_AUTH_REQ_SC_MITM_BOND) {
    esp_ble_gap_cb_param_t gap{}; std::memcpy(gap.ble_security.auth_cmpl.bd_addr, peer, 6);
    gap.ble_security.auth_cmpl.success = success; gap.ble_security.auth_cmpl.auth_mode = mode;
    assert(Client().GapEvent(ESP_GAP_BLE_AUTH_CMPL_EVT, &gap));
}
void Discover(bool include_ancs = true) {
    esp_ble_gattc_cb_param_t result{};
    result.search_res.conn_id = connection; result.search_res.start_handle = 1; result.search_res.end_handle = 15;
    result.search_res.srvc_id.uuid.len = ESP_UUID_LEN_16; result.search_res.srvc_id.uuid.uuid.uuid16 = 0x1801;
    Event(ESP_GATTC_SEARCH_RES_EVT, result);
    if (include_ancs) {
        constexpr uint8_t uuid[]{0xd0,0x00,0x2d,0x12,0x1e,0x4b,0x0f,0xa4,0x99,0x4e,0xce,0xb5,0x31,0xf4,0x05,0x79};
        result.search_res.start_handle = 16; result.search_res.end_handle = 80;
        result.search_res.srvc_id.uuid.len = ESP_UUID_LEN_128;
        std::memcpy(result.search_res.srvc_id.uuid.uuid.uuid128, uuid, 16); Event(ESP_GATTC_SEARCH_RES_EVT, result);
    }
    result = {}; result.search_cmpl.conn_id = connection; result.search_cmpl.status = ESP_GATT_OK;
    Event(ESP_GATTC_SEARCH_CMPL_EVT, result);
}
void RegisterNotify(uint16_t handle) {
    esp_ble_gattc_cb_param_t result{};
    result.reg_for_notify.handle = handle; result.reg_for_notify.status = ESP_GATT_OK;
    Event(ESP_GATTC_REG_FOR_NOTIFY_EVT, result);
}
void WriteAck(uint16_t handle, bool descriptor, esp_gatt_status_t status = ESP_GATT_OK) {
    esp_ble_gattc_cb_param_t result{}; result.write.conn_id = connection; result.write.handle = handle; result.write.status = status;
    Event(descriptor ? ESP_GATTC_WRITE_DESCR_EVT : ESP_GATTC_WRITE_CHAR_EVT, result);
}
void Notify(uint16_t handle, Bytes bytes) {
    esp_ble_gattc_cb_param_t result{}; result.notify.conn_id = connection; result.notify.handle = handle;
    std::memcpy(result.notify.remote_bda, peer, 6);
    result.notify.is_notify = true; result.notify.value_len = bytes.size(); result.notify.value = bytes.data();
    Event(ESP_GATTC_NOTIFY_EVT, result);
}
void Subscribe(bool early = false) {
    RegisterNotify(4); WriteAck(5, true);
    RegisterNotify(20); WriteAck(21, true);
    RegisterNotify(24);
    if (early) {
        Notify(24, {0,0,0,1,0,0,0,0});
        assert(Client().Snapshot().notifications.notifications.empty() && control_writes.empty());
    }
    WriteAck(25, true);
    assert(Client().Snapshot().state == WatchAncsLinkState::kReady);
    assert(Client().NeedsResponsiveLink() == early);
    assert((registrations == std::vector<uint16_t>{4,20,24}));
    assert((descriptor_writes == std::vector<std::pair<uint16_t, Bytes>>{{5,{2,0}}, {21,{1,0}}, {25,{1,0}}}));
}
void Ready(bool early = false) { Reset(); Open(); Number(); Auth(); Discover(); Subscribe(early); }
Bytes Reply() { return {0,0,0,0,0,0,0,0,1,2,0,'H','i',3,2,0,'O','K',5,0,0}; }
bool Called(const char* name) { return std::find(calls.begin(), calls.end(), name) != calls.end(); }
void NoContent() { assert(Client().Snapshot().notifications.notifications.empty()); }
void TestConsentAndAuthentication() {
    Reset(false);
    assert(!Client().NeedsResponsiveLink());
    assert(!Called("open") && !Called("encrypt") && !Called("search"));
    esp_ble_gap_cb_param_t gap{}; std::memcpy(gap.ble_security.ble_req.bd_addr, peer, 6);
    assert(Client().GapEvent(ESP_GAP_BLE_SEC_REQ_EVT, &gap));
    assert(security_replies == std::vector<bool>{false});
    assert(Client().GapEvent(ESP_GAP_BLE_NC_REQ_EVT, &gap));
    assert(confirmations == std::vector<bool>{false});
    assert(!Client().GapEvent(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &gap));
    assert(Client().SetEnabled(true)); confirmations.clear(); Open();
    assert(Client().GapEvent(ESP_GAP_BLE_SEC_REQ_EVT, &gap));
    assert(security_replies.back());
    Number(); Auth(); assert(Called("search"));
    Reset(); Open(); Auth(true, ESP_LE_AUTH_REQ_SC_BOND);
    assert(Client().Snapshot().state == WatchAncsLinkState::kFailed && Called("disconnect"));
    assert(!Called("search"));
    Reset(); Open(); Number(false);
    assert(!Called("search") && Called("disconnect"));
}
void StartDevicePairing(bool notifications = false) {
    Reset(notifications);
    assert(!Client().SelectDevicePairingPeer(connection, peer));
    const auto previous = Client().Snapshot().confirmation;
    device_pairing_window = Client().OpenDevicePairingWindow();
    assert(device_pairing_window);
    assert(!Client().OpenDevicePairingWindow());
    assert(!Client().Confirm(previous, true));
    assert(!Client().SelectAndroidPeer(connection, peer));
    assert(!Client().SelectDevicePairingPeer(connection + 1, peer));
    uint8_t other[]{9, 8, 7, 6, 5, 4};
    assert(!Client().SelectDevicePairingPeer(connection, other));
    assert(Client().SelectDevicePairingPeer(connection, peer));
    assert(!Client().SelectDevicePairingPeer(connection, peer));
    const auto snapshot = Client().Snapshot();
    assert(snapshot.device_pairing_peer && !snapshot.android_peer && snapshot.enabled == notifications);
    assert(snapshot.state == WatchAncsLinkState::kPeerConsent && snapshot.confirmation);
    assert(!Client().Confirm(snapshot.confirmation, true));
    assert(!Client().ConfirmDevicePairing(previous, true));
    assert(!Client().ConfirmDevicePairing(snapshot.confirmation + 1, true));
    assert(!Client().DevicePairingPeerLease(connection, peer));
    assert(Client().ConfirmDevicePairing(snapshot.confirmation, true));
    assert(Client().Snapshot().state == WatchAncsLinkState::kPairing && Called("encrypt"));
    assert(!Called("open") && !Called("search") && !Called("mtu_request"));
}
void DevicePairingNumber(bool accept = true) {
    esp_ble_gap_cb_param_t gap{};
    std::memcpy(gap.ble_security.key_notif.bd_addr, peer, 6); gap.ble_security.key_notif.passkey = 234567;
    assert(Client().GapEvent(ESP_GAP_BLE_NC_REQ_EVT, &gap));
    const auto snapshot = Client().Snapshot();
    assert(snapshot.state == WatchAncsLinkState::kConfirmNumber && snapshot.passkey == 234567);
    assert(!Client().Confirm(snapshot.confirmation, true));
    assert(Client().ConfirmDevicePairing(snapshot.confirmation, accept));
    assert(confirmations.back() == accept);
}
uint64_t DevicePairingReady(bool notifications = false) {
    StartDevicePairing(notifications); DevicePairingNumber(); Auth();
    assert(Client().Snapshot().state == WatchAncsLinkState::kDevicePairingReady);
    const auto lease = Client().DevicePairingPeerLease(connection, peer);
    assert(lease && !Client().DevicePairingPeerLease(connection + 1, peer));
    assert(!Client().AndroidPeerLease(connection, peer) && !Client().RenewAndroidPeer(lease));
    assert(!Called("search") && registrations.empty() && control_writes.empty());
    NoContent();
    return lease;
}
void TestDevicePairingPurposeAndConsent() {
    DevicePairingReady();
    assert(!Client().Snapshot().enabled);
    const auto presentation = BuildWatchNotificationSnapshot(Client().Snapshot());
    assert(!presentation.content.ready && presentation.content.source == WatchNotificationSource::kNone);
    Client().CloseDevicePairingWindow(device_pairing_window);
    assert(Called("disconnect") && !Client().DevicePairingPeerLease(connection, peer));
    assert(!Client().OpenDevicePairingWindow());
    Disconnected(); Connected();
    assert(!Client().SelectDevicePairingPeer(connection, peer));
    assert(!Client().Snapshot().enabled);
    confirmations.clear();
    assert(Client().SetEnabled(true)); Open(); Number(); Auth(); Discover(); Subscribe();
    assert(!Client().OpenDevicePairingWindow());
    assert(Client().Snapshot().state == WatchAncsLinkState::kReady);
}
void TestControlAuthorizationRequiresSecureLocalAdmission() {
    StartDevicePairing();
    const auto pending = Client().Snapshot().link_generation;
    assert(!Client().AuthorizeControlPeer(device_pairing_window, pending));
    DevicePairingNumber();
    assert(!Client().AuthorizeControlPeer(device_pairing_window, pending));
    Auth();
    const auto lease = Client().DevicePairingPeerLease(connection, peer);
    assert(lease && !Client().ControlPeerLease(connection, peer));
    assert(!Client().AuthorizeControlPeer(0, lease));
    assert(!Client().AuthorizeControlPeer(device_pairing_window + 1, lease));
    assert(!Client().AuthorizeControlPeer(device_pairing_window, 0));
    assert(!Client().AuthorizeControlPeer(device_pairing_window, lease + 1));
    assert(Client().AuthorizeControlPeer(device_pairing_window, lease));
    assert(Client().ControlPeerLease(connection, peer) == lease);
    uint8_t other[]{9,8,7,6,5,4};
    assert(!Client().ControlPeerLease(connection + 1, peer));
    assert(!Client().ControlPeerLease(connection, other));
    assert(!Client().ControlPeerLease(connection, nullptr));
    assert(!Client().Snapshot().enabled && !Called("search"));
}
uint64_t StartLocalControlPairing(bool before_connect = false) {
    Reset(false);
    if (before_connect) Disconnected();
    const auto window = Client().OpenControlPairingWindow();
    assert(window && Client().ControlPairingWindowCurrent(window));
    assert(!Client().OpenControlPairingWindow() && !Client().OpenDevicePairingWindow());
    assert(!Client().DevicePairingWindowCurrent(window));
    if (before_connect) Connected();
    const auto snapshot = Client().Snapshot();
    assert(snapshot.control_pairing_peer && !snapshot.device_pairing_peer && !snapshot.enabled);
    assert(snapshot.state == WatchAncsLinkState::kPeerConsent && snapshot.confirmation);
    assert(!Client().Confirm(snapshot.confirmation, true));
    assert(!Client().ConfirmDevicePairing(snapshot.confirmation, true));
    assert(!Client().SelectDevicePairingPeer(connection, peer));
    assert(!Client().DevicePairingPeerLease(connection, peer));
    assert(!Client().AuthorizeControlPeer(window, snapshot.link_generation));
    assert(!Client().ConfirmControlPairing(window + 1, snapshot.confirmation, true));
    assert(!Client().ConfirmControlPairing(window, snapshot.confirmation + 1, true));
    assert(Client().ConfirmControlPairing(window, snapshot.confirmation, true));
    assert(Client().Snapshot().state == WatchAncsLinkState::kPairing);
    return window;
}
void ControlNumber(uint64_t window, bool accept = true) {
    esp_ble_gap_cb_param_t gap{};
    std::memcpy(gap.ble_security.key_notif.bd_addr, peer, 6);
    gap.ble_security.key_notif.passkey = 42;
    Client().GapEvent(ESP_GAP_BLE_NC_REQ_EVT, &gap);
    const auto snapshot = Client().Snapshot();
    assert(snapshot.state == WatchAncsLinkState::kConfirmNumber && snapshot.passkey == 42);
    assert(!Client().ConfirmDevicePairing(snapshot.confirmation, accept));
    assert(Client().ConfirmControlPairing(window, snapshot.confirmation, accept));
}
void TestOfflineControlPairingBeforeAndAfterConnect() {
    for (const bool before_connect : {false, true}) {
        const auto window = StartLocalControlPairing(before_connect);
        BlufiControlSession control(true);
        control.Connected(connection, peer);
        const auto verify = [](uint16_t connection_id, const uint8_t* address) {
            return audream_blufi_control_lease(connection_id, address);
        };
        assert(!control.Capture(verify));
        Client().DisableCollection(); Client().SetEnabled(false); Client().Poll();
        assert(Client().ControlPairingWindowCurrent(window));
        ControlNumber(window);
        assert(!Client().ControlPeerLease(connection, peer));
        Auth();
        const auto lease = Client().ControlPeerLease(connection, peer);
        const auto request = control.Capture(verify);
        assert(request && request.authorization == lease && control.IsCurrent(request, verify));
        assert(lease && Client().Snapshot().state == WatchAncsLinkState::kControlReady);
        assert(Client().ControlPairingLease(window) == lease && !Client().ControlPairingLease(window + 1));
        assert(!Client().ControlPairingWindowCurrent(window));
        assert(!Client().Snapshot().control_pairing_peer);
        assert(!Client().DevicePairingWindowCurrent(window) && !Client().DevicePairingPeerLease(connection, peer));
        assert(!Client().DevicePairingCommitAllowed(window, lease, connection, peer));
        Client().CloseDevicePairingWindow(window); Client().Poll();
        assert(Client().ControlPeerLease(connection, peer) == lease);
        assert(!Client().OpenControlPairingWindow());
        assert(!Called("disconnect") && !Called("search") && registrations.empty());
        assert(!BuildWatchNotificationSnapshot(Client().Snapshot()).content.ready);
        Client().CancelControlPairing(window + 1);
        assert(Client().ControlPeerLease(connection, peer) == lease);
        Client().CancelControlPairing(window);
        assert(!Client().ControlPeerLease(connection, peer) && !Client().ControlPairingLease(window));
        assert(!control.IsCurrent(request, verify) && !control.Capture(verify));
    }
}
void TestOfflineControlPairingFailureAndCancellation() {
    auto window = StartLocalControlPairing();
    ControlNumber(window, false); Auth();
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
    window = StartLocalControlPairing();
    ControlNumber(window); Auth(false);
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
    window = StartLocalControlPairing();
    ControlNumber(window); Auth(true, ESP_LE_AUTH_BOND);
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
    window = StartLocalControlPairing();
    Client().CloseDevicePairingWindow(window); Auth();
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
    window = StartLocalControlPairing();
    Client().CancelControlPairing(window); Auth();
    assert(!Client().ControlPeerLease(connection, peer) && !Client().ControlPairingLease(window));
    window = StartLocalControlPairing();
    clock_us += 60000000; Client().Poll(); Auth();
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
    Reset(false); Disconnected();
    window = Client().OpenControlPairingWindow(); assert(window);
    clock_us += 120000000; Connected(); Auth();
    assert(!Client().ControlPairingWindowCurrent(window));
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
}

void TestControlAuthorizationSurvivesCloudWindow() {
    const auto lease = DevicePairingReady();
    const auto first_window = device_pairing_window;
    assert(Client().AuthorizeControlPeer(first_window, lease));
    Client().CloseDevicePairingWindow(first_window);
    assert(!Called("disconnect"));
    assert(Client().Snapshot().state == WatchAncsLinkState::kControlReady);
    assert(Client().ControlPeerLease(connection, peer) == lease);
    assert(!Client().DevicePairingPeerLease(connection, peer));
    assert(!Client().DevicePairingCommitAllowed(first_window, lease, connection, peer));
    Client().SetEnabled(true); Client().DisableCollection(); Client().SetEnabled(false); Client().Poll();
    assert(!Called("disconnect") && Client().ControlPeerLease(connection, peer) == lease);
    assert(!BuildWatchNotificationSnapshot(Client().Snapshot()).content.ready);
    assert(!Client().Confirm(Client().Snapshot().confirmation, true));
    calls.clear();
    const auto second_window = Client().OpenDevicePairingWindow();
    assert(second_window > first_window);
    Client().CloseDevicePairingWindow(first_window);
    assert(Client().DevicePairingWindowCurrent(second_window));
    assert(Client().SelectDevicePairingPeer(connection, peer));
    assert(Client().Snapshot().state == WatchAncsLinkState::kDevicePairingReady);
    assert(Client().DevicePairingPeerLease(connection, peer) == lease);
    assert(!Called("encrypt") && !Called("disconnect"));
    clock_us += 120000000; Client().Poll();
    assert(!Client().DevicePairingWindowCurrent(second_window));
    assert(!Client().DevicePairingCommitAllowed(second_window, lease, connection, peer));
    assert(!Client().DevicePairingPeerLease(connection, peer));
    assert(Client().ControlPeerLease(connection, peer) == lease && !Called("disconnect"));
    clock_us += 120000000; Client().Poll();
    assert(Client().ControlPeerLease(connection, peer) == lease);
}

void TestControlAuthorizationRevokedWithPhysicalSecurity() {
    auto lease = DevicePairingReady();
    assert(Client().AuthorizeControlPeer(device_pairing_window, lease));
    Client().CloseDevicePairingWindow(device_pairing_window);
    Auth(true, ESP_LE_AUTH_BOND);
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
    lease = DevicePairingReady();
    assert(Client().AuthorizeControlPeer(device_pairing_window, lease));
    Client().CloseDevicePairingWindow(device_pairing_window);
    Disconnected(); Connected(); Auth();
    assert(!Client().ControlPeerLease(connection, peer));
    assert(!Client().AuthorizeControlPeer(device_pairing_window, lease));
    lease = DevicePairingReady();
    assert(Client().AuthorizeControlPeer(device_pairing_window, lease));
    Client().HostStopping();
    assert(!Client().ControlPeerLease(connection, peer));
    Client().HostStopped();
    assert(!Client().ControlPeerLease(connection, peer));
}
void TestDevicePairingNotificationIndependence() {
    const auto lease = DevicePairingReady(true);
    Client().DisableCollection(); Client().Poll();
    assert(!Client().Snapshot().enabled && !Called("disconnect"));
    assert(Client().DevicePairingPeerLease(connection, peer) == lease);
    assert(Client().SetEnabled(false));
    assert(Client().DevicePairingPeerLease(connection, peer) == lease);
    assert(Client().SetEnabled(true));
    Notify(0, {0,0,0,1,0,0,0,0}); Discover();
    assert(Client().DevicePairingPeerLease(connection, peer) == lease);
    assert(registrations.empty() && control_writes.empty());
    assert(!BuildWatchNotificationSnapshot(Client().Snapshot()).content.ready);
    Client().CloseDevicePairingWindow(device_pairing_window);
    assert(Client().Snapshot().enabled && Called("disconnect"));
}
void TestDevicePairingExpiryAndRetirement() {
    DevicePairingReady();
    const auto old_window = device_pairing_window;
    Client().CloseDevicePairingWindow(0);
    Client().CloseDevicePairingWindow(old_window + 1);
    assert(Client().DevicePairingPeerLease(connection, peer));
    clock_us += 60000000; Client().Poll();
    assert(Client().DevicePairingPeerLease(connection, peer));
    clock_us += 60000000; Client().Poll();
    assert(!Client().DevicePairingPeerLease(connection, peer) && Called("disconnect"));
    DevicePairingReady();
    Client().CloseDevicePairingWindow(old_window);
    assert(Client().DevicePairingPeerLease(connection, peer));
    clock_us = 0;
    assert(!Client().DevicePairingPeerLease(connection, peer) && Called("disconnect"));
    StartDevicePairing(); const auto old = Client().Snapshot().confirmation;
    clock_us += 60000000; Client().Poll();
    Auth(); assert(!Client().DevicePairingPeerLease(connection, peer) && Called("disconnect"));
    assert(!Client().ConfirmDevicePairing(old, true));
    DevicePairingReady(); Client().HostStopping();
    assert(!Client().DevicePairingPeerLease(connection, peer));
    DevicePairingReady(); Disconnected(); Connected();
    assert(!Client().DevicePairingPeerLease(connection, peer));
    assert(!Client().SelectDevicePairingPeer(connection, peer));
    Reset(false); Disconnected();
    assert(Client().OpenDevicePairingWindow());
    clock_us += 120000000; Client().Poll(); Connected();
    assert(!Client().SelectDevicePairingPeer(connection, peer));
    assert(!Called("encrypt"));
}
void TestDevicePairingAuthenticationFailures() {
    StartDevicePairing(); DevicePairingNumber(false); Auth();
    assert(!Client().DevicePairingPeerLease(connection, peer) && Called("disconnect"));
    StartDevicePairing(); DevicePairingNumber(); Auth(true, ESP_LE_AUTH_REQ_SC_BOND);
    assert(!Client().DevicePairingPeerLease(connection, peer) && Called("disconnect"));
    StartDevicePairing();
    esp_ble_gap_cb_param_t gap{};
    std::memcpy(gap.ble_security.key_notif.bd_addr, peer, 6); gap.ble_security.key_notif.passkey = 234567;
    Client().GapEvent(ESP_GAP_BLE_NC_REQ_EVT, &gap);
    Auth(); // Authentication cannot bypass a pending numeric comparison.
    assert(!Client().DevicePairingPeerLease(connection, peer) && Called("disconnect"));
    DevicePairingReady(); Auth(false);
    assert(!Client().DevicePairingPeerLease(connection, peer) && Called("disconnect"));
    Reset(false); assert(Client().OpenDevicePairingWindow());
    assert(Client().SelectDevicePairingPeer(connection, peer));
    fail_api = "encrypt";
    assert(!Client().ConfirmDevicePairing(Client().Snapshot().confirmation, true));
    assert(!Client().DevicePairingPeerLease(connection, peer) && Called("disconnect"));
}
void TestEarlyNotificationsAndPrivateLifecycle() {
    Ready(true);
    assert(Client().NeedsResponsiveLink());
    assert(control_writes == std::vector<Bytes>({{0,0,0,0,0,0,1,120,0,3,0,2,5}}));
    Notify(20, Reply());
    assert(!Client().Snapshot().notifications.notifications.front().details_ready);
    WriteAck(30, false);
    assert(!Client().NeedsResponsiveLink());
    auto snapshot = Client().Snapshot();
    assert(snapshot.notifications.notifications.front().title == "Hi");
    assert(snapshot.notifications.notifications.front().message == "OK");
    const auto old_session = snapshot.notifications.session;
    assert(Client().Clear(old_session)); NoContent();
    Notify(24, {0,0,0,1,0,0,0,0}); WriteAck(30, false); Notify(20, Reply());
    assert(Client().Snapshot().notifications.notifications.front().details_ready);
    Client().SetEnabled(false); NoContent(); assert(Called("disconnect"));
    assert((unregistrations == std::vector<uint16_t>{4,20,24}));
    Client().SetEnabled(true); Connected(); Notify(24, {0,0,0,1,0,0,0,0}); NoContent();
    assert(Client().Snapshot().confirmation == 0);
    Disconnected(); ++connection; Connected();
    assert(Client().Snapshot().state == WatchAncsLinkState::kPeerConsent);
    assert(!Client().Clear(old_session));
}
void TestFailuresAndServiceChanges() {
    Ready(); Notify(24, {0,0,0,1,0,0,0,0});
    WriteAck(30, false, ESP_GATT_INSUF_AUTHORIZATION);
    NoContent(); assert(Called("disconnect"));
    Ready(); Notify(24, {0,0,0,1,0,0,0,0});
    WriteAck(30, false, static_cast<esp_gatt_status_t>(0xa2));
    NoContent(); assert(!Called("disconnect") && Client().Snapshot().state == WatchAncsLinkState::kReady);
    Ready(); Notify(24, {0,0,0,1,0,0,0,0}); WriteAck(30, false); Notify(20, Reply());
    Auth(false); NoContent(); assert(Called("disconnect"));
    Ready(); Notify(24, {0,0,0,1,0,0,0,0}); WriteAck(30, false); Notify(20, Reply());
    Notify(4, {1,0,255,255}); NoContent(); assert(Called("disconnect") && Called("cache_refresh"));
    Reset(); Open(); Number(); Auth(); Discover(false);
    RegisterNotify(4); WriteAck(5, true);
    assert(Client().Snapshot().state == WatchAncsLinkState::kUnavailable);
    assert(!Called("disconnect") && control_writes.empty());
    esp_ble_gattc_cb_param_t change{}; std::memcpy(change.srvc_chg.remote_bda, peer, 6);
    Event(ESP_GATTC_SRVC_CHG_EVT, change); assert(Called("disconnect"));
}
void TestBoundsTimeoutsAndHostRetirement() {
    Reset(); const auto token = Client().Snapshot().confirmation;
    clock_us += 60000000; Client().Poll();
    assert(Called("disconnect") && !Client().Confirm(token, true));
    Reset(); Open(); clock_us += 60000000; Auth();
    assert(Called("disconnect") && !Called("search"));
    Ready(); Notify(24, {0,0,0,1,0,0,0,0});
    clock_us += 5000000; Client().Poll(); NoContent(); assert(Called("disconnect"));
    Reset(); Open(); Number(); Auth(); Discover();
    RegisterNotify(4); WriteAck(5, true); RegisterNotify(20); WriteAck(21, true); RegisterNotify(24);
    for (int i = 0; i < 21; ++i) Notify(24, {0,0,0,1,static_cast<uint8_t>(i),0,0,0});
    NoContent(); assert(Called("disconnect"));
    WriteAck(25, true); assert(control_writes.empty());
    Ready(true); Client().HostStopping(); NoContent();
    WriteAck(30, false); Notify(20, Reply()); NoContent();
    Client().HostStopped(); assert(Client().Snapshot().state == WatchAncsLinkState::kOffline);
    Reset(); Open(); Auth(); Discover();
    // Complete a notification registration only after its connection retired.
    Disconnected(); RegisterNotify(4);
    assert((unregistrations == std::vector<uint16_t>{4}) && descriptor_writes.empty());
}
void TestNativeApiFailures() {
    for (const auto& name : {"open", "mtu_request", "encrypt", "search", "register_notify", "get_descriptor", "write_descriptor", "write_char"}) {
        Reset(); fail_api = name;
        const auto token = Client().Snapshot().confirmation;
        const bool opened = Client().Confirm(token, true);
        if (name == std::string("open")) assert(!opened);
        else {
            esp_ble_gattc_cb_param_t value{}; value.open.conn_id = connection; value.open.status = ESP_GATT_OK;
            std::memcpy(value.open.remote_bda, peer, 6); Event(ESP_GATTC_OPEN_EVT, value);
            if (!Called("disconnect")) {
                value = {}; value.cfg_mtu.conn_id = connection; value.cfg_mtu.status = ESP_GATT_OK; value.cfg_mtu.mtu = 512;
                Event(ESP_GATTC_CFG_MTU_EVT, value);
            }
            if (!Called("disconnect")) { Auth(); Discover(); }
            if (!Called("disconnect")) { RegisterNotify(4); WriteAck(5, true); }
            if (!Called("disconnect")) { RegisterNotify(20); WriteAck(21, true); RegisterNotify(24); WriteAck(25, true); }
            if (!Called("disconnect")) Notify(24, {0,0,0,1,0,0,0,0});
        }
        NoContent(); assert(Called("disconnect"));
    }
    Reset(); Open(); Auth(); missing_control = true; Discover();
    assert(Client().Snapshot().state == WatchAncsLinkState::kUnavailable && registrations.empty());
    Ready(); fail_api = "unregister_notify";
    Client().SetEnabled(false); Disconnected(); Client().SetEnabled(true); ++connection; Connected();
    assert(Client().Snapshot().state == WatchAncsLinkState::kFailed && !Client().Snapshot().confirmation);
    NoContent();
}
void TestMemoryOnlyOptOut() {
    Ready(true); WriteAck(30, false); Notify(20, Reply());
    const auto before = calls.size();
    const auto session = Client().Snapshot().notifications.session;
    Client().DisableCollection();
    assert(calls.size() == before);
    assert(!Client().Snapshot().enabled && !Client().Snapshot().confirmation);
    NoContent();
    Notify(24, {0,0,0,1,0,0,0,0}); Notify(20, Reply()); WriteAck(30, false);
    assert(calls.size() == before);
    NoContent(); assert(!Client().Clear(session));
    Client().Poll(); assert(Called("disconnect"));
    assert((unregistrations == std::vector<uint16_t>{4,20,24}));
    Client().SetEnabled(true);
    assert(!Client().Snapshot().confirmation);
    Disconnected(); Connected();
    assert(Client().Snapshot().confirmation);

    Reset(); const auto token = Client().Snapshot().confirmation;
    const auto pairing_before = calls.size();
    Client().DisableCollection();
    assert(calls.size() == pairing_before && !Client().Confirm(token, true));
    Client().SetEnabled(true);
    assert(Called("disconnect") && !Client().Snapshot().confirmation);
}
void ServerEvent(esp_gatts_cb_event_t event, esp_ble_gatts_cb_param_t& value) {
    assert(server_callback); server_callback(event, kServerInterface, &value);
}
void ServerMtu(uint16_t mtu) {
    esp_ble_gatts_cb_param_t value{}; value.mtu.conn_id = connection; value.mtu.mtu = mtu;
    ServerEvent(ESP_GATTS_MTU_EVT, value);
}
void StartServer() {
    assert(Server().HostStarted()); Server().SetEnabled(true);
    esp_ble_gatts_cb_param_t value{}; value.reg.app_id = 0x2da2; value.reg.status = ESP_GATT_OK;
    ServerEvent(ESP_GATTS_REG_EVT, value);
    uint16_t handles[]{100,101,102,103,104,105,106,107,108,109};
    value = {}; value.add_attr_tab.status = ESP_GATT_OK; value.add_attr_tab.num_handle = 10;
    value.add_attr_tab.handles = handles; value.add_attr_tab.svc_uuid.len = ESP_UUID_LEN_128;
    std::copy(server_uuid.begin(), server_uuid.end(), value.add_attr_tab.svc_uuid.uuid.uuid128);
    ServerEvent(ESP_GATTS_CREAT_ATTR_TAB_EVT, value);
    value = {}; value.start.status = ESP_GATT_OK; value.start.service_handle = 100; ServerEvent(ESP_GATTS_START_EVT, value);
    value = {}; value.connect.conn_id = connection; value.connect.link_role = 1;
    std::copy_n(peer, 6, value.connect.remote_bda); ServerEvent(ESP_GATTS_CONNECT_EVT, value);
    ServerMtu(185);
}
void PairingEvent(esp_gatts_cb_event_t event, esp_ble_gatts_cb_param_t& value) {
    assert(server_callback); server_callback(event, kPairingInterface, &value);
}
void ServerRead(uint16_t handle);
void StartPairingServer() {
    assert(PairingServer().HostStarted());
    esp_ble_gatts_cb_param_t value{}; value.reg.app_id = 0x2da3; value.reg.status = ESP_GATT_OK;
    PairingEvent(ESP_GATTS_REG_EVT, value);
    uint16_t handles[]{200,201,202,203,204,205,206};
    value = {}; value.add_attr_tab.status = ESP_GATT_OK; value.add_attr_tab.num_handle = 7;
    value.add_attr_tab.handles = handles; value.add_attr_tab.svc_uuid.len = ESP_UUID_LEN_128;
    std::copy(pairing_uuid.begin(), pairing_uuid.end(), value.add_attr_tab.svc_uuid.uuid.uuid128);
    PairingEvent(ESP_GATTS_CREAT_ATTR_TAB_EVT, value);
    value = {}; value.start.status = ESP_GATT_OK; value.start.service_handle = 200; PairingEvent(ESP_GATTS_START_EVT, value);
    value = {}; value.connect.conn_id = connection; value.connect.link_role = 1;
    std::copy_n(peer, 6, value.connect.remote_bda); PairingEvent(ESP_GATTS_CONNECT_EVT, value);
    value = {}; value.mtu.conn_id = connection; value.mtu.mtu = 185; PairingEvent(ESP_GATTS_MTU_EVT, value);
}
void PairingWrite(uint16_t handle, Bytes bytes, bool prepare = false) {
    esp_ble_gatts_cb_param_t value{}; value.write.conn_id = connection; value.write.trans_id = 22; value.write.handle = handle;
    value.write.need_rsp = true; value.write.is_prep = prepare; value.write.len = bytes.size(); value.write.value = bytes.data();
    std::copy_n(peer, 6, value.write.bda); PairingEvent(ESP_GATTS_WRITE_EVT, value);
}
void PairingRead(uint16_t handle, uint16_t offset = 0) {
    esp_ble_gatts_cb_param_t value{}; value.read.conn_id = connection; value.read.trans_id = 23;
    value.read.handle = handle; value.read.need_rsp = true; value.read.offset = offset;
    std::copy_n(peer, 6, value.read.bda); PairingEvent(ESP_GATTS_READ_EVT, value);
}
Bytes PairingFrame(uint8_t kind, uint64_t request, uint16_t total, uint16_t offset, std::string payload) {
    Bytes frame{1,kind};
    for (size_t i = 0; i < 8; ++i) frame.push_back(static_cast<uint8_t>(request >> (8 * i)));
    frame.push_back(total & 255); frame.push_back(total >> 8);
    frame.push_back(offset & 255); frame.push_back(offset >> 8);
    frame.insert(frame.end(), payload.begin(), payload.end()); return frame;
}
void PairingTransportReady(bool notifications = false) {
    Reset(notifications); StartPairingServer();
    assert(!PairingServer().OpenLocalWindow(1, bound_device));
    PairingRead(202); assert(pairing_responses.back().first == ESP_GATT_OK && pairing_responses.back().second[1] == 0);
    device_pairing_window = Client().OpenDevicePairingWindow(); assert(device_pairing_window);
    assert(PairingServer().OpenLocalWindow(device_pairing_window, bound_device));
    assert(!PairingServer().OpenLocalWindow(device_pairing_window, bound_device));
    Bytes select{1};
    for (size_t i = 0; i < 8; ++i) select.push_back(static_cast<uint8_t>(device_pairing_window >> (8 * i)));
    PairingRead(202); assert(pairing_responses.back().second[1] == 1 && pairing_responses.back().second.size() == 46);
    PairingWrite(202, select); assert(pairing_responses.back().first == ESP_GATT_OK);
    PairingWrite(204, PairingFrame(1, 7, 5, 0, "hello"));
    assert(pairing_responses.back().first == ESP_GATT_INSUF_AUTHORIZATION && !PairingServer().TakePending());
    assert(Client().ConfirmDevicePairing(Client().Snapshot().confirmation, true));
    DevicePairingNumber(); Auth();
    assert(Client().DevicePairingPeerLease(connection, peer));
}
WatchPairingGattServer::Request TransportChallenge() {
    PairingWrite(204, PairingFrame(1, 7, 200, 0, std::string(160, 'a')));
    assert(pairing_responses.back().first == ESP_GATT_OK && !PairingServer().TakePending());
    PairingWrite(204, PairingFrame(1, 7, 200, 160, std::string(40, 'b')));
    assert(pairing_responses.back().first == ESP_GATT_OK);
    auto request = PairingServer().TakePending();
    assert(request && request->kind == WatchPairingGattServer::Kind::kChallenge && request->payload.size() == 200);
    assert(request->payload == std::string(160, 'a') + std::string(40, 'b'));
    assert(!PairingServer().TakePending()); return *request;
}
void TestPairingGattWorkerRoundTrip() {
    PairingTransportReady(); const auto request = TransportChallenge();
    using Stage = WatchPairingGattServer::Stage;
    PairingRead(206); assert(pairing_responses.back().second[1] == static_cast<uint8_t>(Stage::kChallengeProcessing));
    std::array<uint8_t, 64> proof{}; proof.fill(9);
    auto stale = request; ++stale.window;
    assert(!PairingServer().PublishProof(stale, proof));
    assert(!PairingServer().BindingCommitted(request));
    assert(PairingServer().PublishProof(request, proof));
    PairingRead(206);
    assert(pairing_responses.back().second[1] == static_cast<uint8_t>(Stage::kProofReady));
    assert(std::equal(proof.begin(), proof.end(), pairing_responses.back().second.begin() + 10));
    PairingWrite(204, PairingFrame(2, 7, bound_credential.size(), 0, bound_credential));
    auto credential = PairingServer().TakePending();
    assert(credential && credential->kind == WatchPairingGattServer::Kind::kCredential && credential->payload == bound_credential);
    PairingServer().Reject(request);
    assert(Client().DevicePairingPeerLease(connection, peer));
    PairingRead(206); assert(pairing_responses.back().second[1] == static_cast<uint8_t>(Stage::kCredentialProcessing));
    assert(std::all_of(pairing_responses.back().second.begin() + 10, pairing_responses.back().second.end(), [](auto b) { return b == 0; }));
    assert(!PairingServer().BindingCommitted(stale));
    assert(!PairingServer().BindingCommitted(*credential));
    assert(!PairingServer().AdmitCredentialCommit(stale));
    assert(PairingServer().RequestCurrent(*credential));
    assert(PairingServer().AdmitCredentialCommit(*credential));
    assert(!PairingServer().RequestCurrent(*credential));
    assert(!PairingServer().AdmitCredentialCommit(*credential));
    assert(PairingServer().BindingCommitted(*credential));
    PairingRead(206); assert(pairing_responses.back().second[1] == static_cast<uint8_t>(Stage::kBound));
    assert(!Client().Snapshot().enabled && !Called("search"));
    PairingServer().CloseLocalWindow(request.window);
    assert(!Client().DevicePairingPeerLease(connection, peer));
}
void TestPairingGattReusesAuthorizedControlConnection() {
    PairingTransportReady();
    const auto lease = Client().DevicePairingPeerLease(connection, peer);
    assert(Client().AuthorizeControlPeer(device_pairing_window, lease));
    const auto first = TransportChallenge();
    PairingServer().Reject(first);
    assert(Client().ControlPeerLease(connection, peer) == lease && !Called("disconnect"));
    assert(!PairingServer().RequestCurrent(first));
    const auto second_window = Client().OpenDevicePairingWindow();
    assert(second_window > device_pairing_window);
    assert(PairingServer().OpenLocalWindow(second_window, bound_device));
    Bytes select{1};
    for (size_t i = 0; i < 8; ++i) select.push_back(static_cast<uint8_t>(second_window >> (8 * i)));
    calls.clear();
    PairingWrite(202, select);
    assert(pairing_responses.back().first == ESP_GATT_OK);
    assert(Client().Snapshot().confirmation == 0 && !Called("encrypt"));
    const auto second = TransportChallenge();
    assert(second.window != first.window && second.peer_lease == first.peer_lease);
    assert(!PairingServer().RequestCurrent(first));
    assert(PairingServer().RequestCurrent(second));
    PairingServer().Reject(second);
    assert(Client().ControlPeerLease(connection, peer) == lease && !Called("disconnect"));
}

int RunMobilePairingTransport() {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    bond_fixture::Reset(peer);
#endif
    PairingTransportReady();
    const auto lease = Client().DevicePairingPeerLease(connection, peer);
    assert(Client().AuthorizeControlPeer(device_pairing_window, lease));
    Client().Poll();
    assert(Client().ControlPeerLease(connection, peer) == lease);
    PairingServer().Reject(TransportChallenge());
    const auto window = Client().OpenDevicePairingWindow();
    assert(window && PairingServer().OpenLocalWindow(window, bound_device));
    std::optional<WatchPairingGattServer::Request> committing;
    std::string expected, line;
    const auto decode = [](const std::string& text) {
        assert(text.size() % 2 == 0 && text.size() <= 4096);
        Bytes bytes;
        for (size_t i = 0; i < text.size(); i += 2) {
            size_t used = 0;
            const auto value = std::stoul(text.substr(i, 2), &used, 16);
            assert(used == 2 && value <= 255);
            bytes.push_back(static_cast<uint8_t>(value));
        }
        return bytes;
    };
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string operation, kind, hex;
        input >> operation >> kind >> hex;
        if (operation == "quit") break;
        if (operation == "expect") {
            const auto bytes = decode(kind);
            expected.assign(bytes.begin(), bytes.end());
            std::cout << "PAIR 0 -" << std::endl;
            continue;
        }
        const auto before = pairing_responses.size();
        if (operation == "write") {
            assert(kind == "hello" || kind == "input");
            PairingWrite(kind == "hello" ? 202 : 204, decode(hex));
        } else {
            assert(operation == "read" && (kind == "hello" || kind == "status"));
            auto request = kind == "status" ? PairingServer().TakePending() : std::nullopt;
            if (request) assert(request->payload == expected);
            PairingRead(kind == "hello" ? 202 : 206);
            // Simulate only the application worker outcome. GATT framing, stage
            // transitions and native peer authorization remain production code.
            if (request && request->kind == WatchPairingGattServer::Kind::kChallenge) {
                std::array<uint8_t, 64> proof{}; proof.fill(9);
                assert(PairingServer().PublishProof(*request, proof));
            } else if (request) {
                assert(PairingServer().AdmitCredentialCommit(*request));
                committing = request;
            } else if (committing) {
                assert(PairingServer().BindingCommitted(*committing));
                committing.reset();
            }
        }
        assert(pairing_responses.size() == before + 1);
        const auto& response = pairing_responses.back();
        std::cout << "PAIR " << static_cast<unsigned>(response.first) << ' ';
        if (response.second.empty()) std::cout << '-';
        else for (const auto byte : response.second) {
            constexpr char digits[] = "0123456789abcdef";
            std::cout << digits[byte >> 4] << digits[byte & 15];
        }
        std::cout << std::endl;
    }
    PairingServer().CloseLocalWindow(window);
    assert(Client().ControlPeerLease(connection, peer) == lease && !Called("disconnect"));
    return 0;
}
void TestPairingGattMalformedAndExpiredData() {
    for (const auto& frame : {PairingFrame(1, 7, 1025, 0, "x"), PairingFrame(2, 7, 10, 0, "x"),
             PairingFrame(1, 0, 10, 0, "x"), PairingFrame(1, 7, 10, 1, "x"), Bytes{1,1}}) {
        PairingTransportReady(); PairingWrite(204, frame);
        assert(pairing_responses.back().first == ESP_GATT_INVALID_ATTR_LEN && !PairingServer().TakePending());
        assert(!Client().DevicePairingPeerLease(connection, peer));
    }
    PairingTransportReady(); PairingWrite(204, PairingFrame(1, 7, 20, 0, "abc"));
    clock_us += 10000000; PairingServer().Poll();
    assert(!PairingServer().TakePending() && !Client().DevicePairingPeerLease(connection, peer));
    PairingTransportReady(); auto request = TransportChallenge();
    Client().CloseDevicePairingWindow(request.window);
    assert(!PairingServer().PublishProof(request, {}));
    PairingTransportReady(); request = TransportChallenge();
    Disconnected(); assert(!PairingServer().PublishProof(request, {}));
    PairingTransportReady(); request = TransportChallenge();
    PairingServer().HostStopping(); assert(!PairingServer().PublishProof(request, {}));
    PairingTransportReady(); PairingWrite(204, PairingFrame(1, 7, 3, 0, "abc"), true);
    assert(!PairingServer().TakePending() && !Client().DevicePairingPeerLease(connection, peer));
    PairingTransportReady(); PairingWrite(204, PairingFrame(1, 7, 20, 0, "abc"));
    PairingWrite(204, PairingFrame(1, 7, 20, 0, "abc"));
    assert(!PairingServer().TakePending() && !Client().DevicePairingPeerLease(connection, peer));
    PairingTransportReady(); PairingRead(206, 1);
    assert(pairing_responses.back().first == ESP_GATT_INVALID_OFFSET);
    request = TransportChallenge();
    PairingServer().Reject(request); assert(!Client().DevicePairingPeerLease(connection, peer));
    for (const bool expired : {false, true}) {
        PairingTransportReady(); request = TransportChallenge();
        assert(PairingServer().PublishProof(request, {}));
        PairingWrite(204, PairingFrame(2, 7, bound_credential.size(), 0, bound_credential));
        auto credential = PairingServer().TakePending(); assert(credential);
        auto stale = *credential; ++stale.peer_lease;
        assert(!PairingServer().AdmitCredentialCommit(stale));
        if (expired) clock_us += 120000000;
        else Disconnected();
        const auto calls_before = calls.size();
        assert(!PairingServer().AdmitCredentialCommit(*credential));
        assert(calls.size() == calls_before);
    }
}
void TestPairingGattSharesDispatcher() {
    PairingTransportReady(true); StartServer();
    // Starting the second service must not overwrite the first callback.
    const auto request = TransportChallenge();
    assert(PairingServer().PublishProof(request, {}));
    PairingRead(206); assert(pairing_responses.back().first == ESP_GATT_OK);
    assert(server_responses.empty());
    PairingServer().CloseLocalWindow(request.window);
    Reset(); StartServer(); StartPairingServer();
    const auto before = pairing_responses.size();
    ServerRead(102); assert(server_responses.back().first == ESP_GATT_INSUF_AUTHORIZATION);
    assert(pairing_responses.size() == before);
    const auto count = server_responses.size();
    esp_ble_gatts_cb_param_t event{}; event.write.conn_id = connection; event.write.need_rsp = true;
    server_callback(ESP_GATTS_WRITE_EVT, ESP_GATT_IF_NONE, &event);
    assert(pairing_responses.size() == before && server_responses.size() == count);
    assert(!RegisterWatchGattCallback(0x2da2, nullptr));
    assert(!RegisterWatchGattCallback(0x2da2, [](esp_gatts_cb_event_t, esp_gatt_if_t, esp_ble_gatts_cb_param_t*) {}));
}
void ServerWrite(uint16_t handle, Bytes bytes, bool prepare = false, bool response = true) {
    esp_ble_gatts_cb_param_t value{};
    value.write.conn_id = connection; value.write.trans_id = 12; value.write.handle = handle;
    value.write.need_rsp = response; value.write.is_prep = prepare; value.write.len = bytes.size(); value.write.value = bytes.data();
    std::copy_n(peer, 6, value.write.bda); ServerEvent(ESP_GATTS_WRITE_EVT, value);
}
void ServerRead(uint16_t handle) {
    esp_ble_gatts_cb_param_t value{}; value.read.conn_id = connection; value.read.trans_id = 13;
    value.read.handle = handle; value.read.need_rsp = true; std::copy_n(peer, 6, value.read.bda);
    ServerEvent(ESP_GATTS_READ_EVT, value);
}
void AckConfirmation(esp_gatt_status_t status = ESP_GATT_OK) {
    esp_ble_gatts_cb_param_t value{}; value.conf.conn_id = connection; value.conf.handle = 106; value.conf.status = status;
    ServerEvent(ESP_GATTS_CONF_EVT, value);
}
Bytes AndroidFrame(uint64_t request = 1, uint64_t revision = 1) {
    Bytes bytes{'A','W','N','1',1,0,0,0};
    auto put = [&](uint64_t value, size_t size) { for (size_t i = 0; i < size; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i))); };
    put(7, 8); put(request, 8); put(revision, 8); put(0, 4);
    put(1, 1); put(1, 4); put(1000, 8);
    for (const std::string text : {"ai.messages", "Messages", "Hi", "Body"}) {
        put(text.size(), 2); bytes.insert(bytes.end(), text.begin(), text.end());
    }
    bytes[32] = static_cast<uint8_t>(bytes.size() - 36);
    return bytes;
}
WatchAndroidGattServer::OwnerRequest AndroidPaired() {
    Reset(); StartServer();
    auto old_confirmation = Client().Snapshot().confirmation;
    ServerWrite(102, {1,7,0,0,0,0,0,0,0});
    assert(server_responses.back().first == ESP_GATT_OK);
    assert(!Client().Confirm(old_confirmation, true));
    assert(Client().Confirm(Client().Snapshot().confirmation, true));
    assert(Client().Snapshot().state == WatchAncsLinkState::kPairing);
    Number(); Auth();
    assert(!Called("search") && !Called("open") && !Called("mtu_request"));
    assert(Client().Snapshot().state == WatchAncsLinkState::kAndroidAuthorizing);
    assert(!Server().PendingOwnerAuthorization());
    ServerWrite(107, {2,0});
    assert(server_responses.back().first == ESP_GATT_OK);
    auto request = Server().PendingOwnerAuthorization(); assert(request && request->peer_lease);
    ServerRead(102); assert(server_responses.back().second.size() == 74 && server_responses.back().second[0] == 2 && server_responses.back().second[1] == 0);
    assert(std::all_of(server_responses.back().second.begin() + 42, server_responses.back().second.end(), [](uint8_t byte) { return byte == 0; }));
    assert(std::equal(request->challenge.begin(), request->challenge.end(), server_responses.back().second.begin() + 10));
    return *request;
}
void AndroidReady() {
    const auto request = AndroidPaired();
    // This injects a completed external owner verification, not a BLE proof.
    assert(ApplyOwner(request));
    assert(!ApplyOwner(request));
    ServerRead(102); assert(server_responses.back().second[1] == 1);
}
void TestAndroidNativeSecurityBoundaries() {
    Reset(); StartServer();
    ServerWrite(104, AndroidFrame()); assert(server_responses.back().first == ESP_GATT_INSUF_AUTHORIZATION);
    ServerRead(102); assert(server_responses.back().first == ESP_GATT_INSUF_AUTHORIZATION);
    assert(!Server().PendingOwnerAuthorization()); assert(server_indications.empty());
    auto request = AndroidPaired();
    ServerWrite(104, AndroidFrame()); assert(server_responses.back().first == ESP_GATT_INSUF_AUTHORIZATION);
    assert(Server().Snapshot().entries.empty() && server_indications.empty());
    auto wrong = request; ++wrong.generation; assert(!ApplyOwner(wrong));
    wrong = request; ++wrong.wire_session; assert(!ApplyOwner(wrong));
    wrong = request; ++wrong.peer_lease; assert(!ApplyOwner(wrong));
    wrong = request; wrong.challenge[0] ^= 1; assert(!ApplyOwner(wrong));
    wrong = request; wrong.peer[0] ^= 1; assert(!ApplyOwner(wrong));
    wrong = request; ++wrong.binding_generation; assert(!ApplyOwner(wrong));
    assert(ApplyOwner(request));
    Auth(false); Server().Poll();
    assert(Server().Snapshot().entries.empty() && Called("disconnect"));
    const auto next = AndroidPaired(); assert(next.generation != request.generation && next.challenge != request.challenge);
    assert(!ApplyOwner(request));
    assert(!Server().RevokeOwnerAuthorization(request));
    Auth(true, ESP_LE_AUTH_REQ_SC_BOND); assert(!ApplyOwner(next));
    request = AndroidPaired(); assert(ApplyOwner(request));
    ServerWrite(104, AndroidFrame()); assert(!Server().Snapshot().entries.empty());
    assert(Server().RevokeOwnerAuthorization(request));
    assert(Server().Snapshot().entries.empty() && Called("disconnect"));
}
void TestAndroidApplyAcksAndLocalState() {
    AndroidReady(); auto bytes = AndroidFrame();
    ServerWrite(104, Bytes(bytes.begin(), bytes.begin() + 20));
    assert(server_indications.empty() && Server().Snapshot().entries.empty());
    ServerWrite(104, Bytes(bytes.begin() + 20, bytes.end()));
    assert(server_indications.size() == 1 && server_indications[0].size() == 37);
    assert(server_indications[0][4] == 2 && server_indications[0][36] == 0);
    auto snapshot = Server().Snapshot(); assert(snapshot.entries.size() == 1 && snapshot.entries[0].title == "Hi");
    const auto client = Client().Snapshot();
    assert(client.android_peer && client.link_generation && snapshot.peer_lease == client.link_generation);
    auto display = BuildWatchNotificationSnapshot(client, snapshot);
    assert(display.content.ready && display.content.source == WatchNotificationSource::kAndroid);
    assert(display.content.session == snapshot.session && display.content.items.front().title == "Hi");
    auto stale = snapshot; ++stale.peer_lease;
    assert(BuildWatchNotificationSnapshot(client, stale).content.items.empty());
    assert(Server().MarkRead(snapshot.session, 1, snapshot.entries[0].revision));
    assert(Server().Snapshot().entries[0].read);
    assert(BuildWatchNotificationSnapshot(Client().Snapshot(), Server().Snapshot()).content.items.front().read);
    AckConfirmation(); assert(Server().Clear(snapshot.session));
    ServerWrite(104, AndroidFrame(2, 2));
    assert(Server().Snapshot().entries.empty() && server_indications.size() == 2);
    AckConfirmation();
    ServerWrite(107, {0,0}); assert(Called("disconnect"));
    assert(Server().Snapshot().entries.empty());
    assert(Server().Snapshot().peer_lease == 0);
    assert(!BuildWatchNotificationSnapshot(Client().Snapshot(), Server().Snapshot()).content.ready);
}
void TestAndroidLimitsTimeoutsAndOptOut() {
    auto request = AndroidPaired(); ServerMtu(23); assert(!ApplyOwner(request));
    ServerRead(102); assert(server_responses.back().first == ESP_GATT_INVALID_ATTR_LEN);
    ServerMtu(185); assert(ApplyOwner(request));
    ServerWrite(104, AndroidFrame(), true); assert(Called("disconnect")); assert(Server().Snapshot().entries.empty());
    AndroidReady(); ServerWrite(104, AndroidFrame(), false, false); assert(Called("disconnect"));
    AndroidReady(); ServerWrite(104, AndroidFrame()); clock_us += 5000000; Server().Poll();
    assert(Server().Snapshot().entries.empty() && Called("disconnect"));
    AndroidReady(); ServerWrite(104, AndroidFrame()); AckConfirmation(); clock_us += 30000000; Server().Poll();
    assert(Server().Snapshot().entries.empty() && Called("disconnect"));
    AndroidPaired(); clock_us += 60000000; Server().Poll(); assert(!Server().PendingOwnerAuthorization() && Called("disconnect"));
    AndroidReady(); ServerWrite(104, AndroidFrame());
    const auto before = calls.size(); Server().DisableCollection();
    assert(calls.size() == before && Server().Snapshot().entries.empty());
    // Snapshot is a polling owner operation and may retire after the pure fence.
    assert(Called("disconnect")); Server().SetEnabled(true);
    assert(!Server().PendingOwnerAuthorization());
}
void TestAndroidNativeFailuresAndIsolation() {
    for (const auto name : {"server_callback", "server_register"}) {
        Reset(); fail_api = name; assert(!Server().HostStarted());
        assert(!Server().PendingOwnerAuthorization() && server_indications.empty());
    }
    for (const auto name : {"server_attributes", "server_start"}) {
        Reset(); fail_api = name; StartServer();
        assert(!Server().HostStarted() && !Server().PendingOwnerAuthorization() && server_indications.empty());
    }
    for (const auto name : {"server_response", "server_indicate"}) {
        AndroidReady(); fail_api = name; ServerWrite(104, AndroidFrame());
        assert(Server().Snapshot().entries.empty() && Called("disconnect"));
    }
    AndroidReady(); ServerWrite(104, AndroidFrame()); AckConfirmation(ESP_GATT_ERROR);
    assert(Server().Snapshot().entries.empty() && Called("disconnect"));
    AndroidReady(); auto data = AndroidFrame();
    esp_ble_gatts_cb_param_t value{}; value.write.conn_id = connection + 1; value.write.trans_id = 99;
    value.write.handle = 104; value.write.need_rsp = true; value.write.len = data.size(); value.write.value = data.data();
    std::copy_n(peer, 6, value.write.bda);
    const auto count = server_responses.size(); ServerEvent(ESP_GATTS_WRITE_EVT, value);
    assert(server_responses.size() == count && Server().Snapshot().entries.empty());
    value.write.conn_id = connection; value.write.bda[0] ^= 1; ServerEvent(ESP_GATTS_WRITE_EVT, value);
    assert(server_responses.size() == count);
    // Anonymous delayed response results cannot revoke a replacement connection.
    value = {}; value.rsp.status = ESP_GATT_ERROR; value.rsp.handle = 104; ServerEvent(ESP_GATTS_RESPONSE_EVT, value);
    assert(!Called("disconnect"));
    Server().HostStopping(); ServerWrite(104, data); assert(Server().Snapshot().entries.empty());
    Server().HostStopped();
    // The added service must not retire an ordinary iPhone ANCS connection when disabled.
    Ready(); StartServer(); const auto before = calls.size(); Server().SetEnabled(false);
    assert(calls.size() == before && Client().Snapshot().state == WatchAncsLinkState::kReady);
}
void SendProof(const Bytes& signed_proof) {
    Bytes proof(32, 0x34); proof.insert(proof.end(), signed_proof.begin(), signed_proof.end());
    for (size_t offset = 0; offset < proof.size(); offset += 160) {
        ServerWrite(109, Bytes(proof.begin() + offset, proof.begin() + std::min(proof.size(), offset + 160)));
        assert(server_responses.back().first == ESP_GATT_OK);
    }
}
void TestAndroidOwnerLeaseAndProofBounds() {
    Reset(); StartServer(); binding_generation = 0;
    ServerWrite(102, {1,7,0,0,0,0,0,0,0});
    assert(server_responses.back().first == ESP_GATT_INSUF_AUTHORIZATION && !Server().PendingOwnerAuthorization());
    auto request = AndroidPaired();
    assert(!Server().ApplyOwnerAuthorization(request, 0, 300, {}));
    assert(!Server().ApplyOwnerAuthorization(request, 2, 300, {}));
    assert(!Server().ApplyOwnerAuthorization(request, 1, 0, {}));
    assert(!Server().ApplyOwnerAuthorization(request, 1, 301, {}));
    assert(ApplyOwner(request));
    ServerWrite(104, AndroidFrame()); AckConfirmation();
    const auto local_session = Server().Snapshot().session;
    for (unsigned i = 1; i <= 29; ++i) {
        clock_us += 10000000; ServerWrite(104, AndroidFrame(i + 1, i + 1)); AckConfirmation();
        assert(!Called("disconnect"));
    }
    // Heartbeats/replacements do not extend the independently bounded owner grant.
    clock_us += 10000000; Server().Poll();
    assert(Called("disconnect") && Server().Snapshot().entries.empty());

    request = AndroidPaired(); assert(ApplyOwner(request));
    ServerWrite(104, AndroidFrame()); AckConfirmation(); clock_us += 10000000;
    ServerWrite(102, {2,7,0,0,0,0,0,0,0});
    auto renewed = Server().PendingOwnerAuthorization();
    assert(renewed && renewed->challenge != request.challenge && renewed->generation == request.generation);
    ServerRead(102); assert(server_responses.back().second[1] == 0);
    const auto before = Server().Snapshot(); assert(before.session != local_session && before.entries.size() == 1);
    assert(!ApplyOwner(request) && !Server().RevokeOwnerAuthorization(request));
    assert(ApplyOwner(*renewed));
    assert(Server().Snapshot().session == before.session && Server().Snapshot().entries.size() == 1);
    ++binding_generation; assert(Server().Snapshot().entries.empty() && Called("disconnect"));

    AndroidPaired(); ++binding_generation;
    assert(!Server().PendingOwnerAuthorization() && Called("disconnect"));

    AndroidPaired(); Bytes invalid(350, 0); invalid[93] = 1;
    SendProof(invalid); assert(Server().HasPendingOwnerProof());
    ServerWrite(104, AndroidFrame()); assert(server_responses.back().first == ESP_GATT_INSUF_AUTHORIZATION);
    Server().ProcessOwnerProof(); assert(!Server().HasPendingOwnerProof() && Called("disconnect"));
    AndroidPaired(); ServerWrite(109, Bytes(126, 0));
    assert(server_responses.back().first == ESP_GATT_INVALID_PDU && Called("disconnect"));
    AndroidPaired(); ServerWrite(109, Bytes(10, 0)); clock_us += 5000000;
    Server().Poll(); assert(!Server().HasPendingOwnerProof() && Called("disconnect"));
    AndroidPaired(); SendProof(invalid); Server().DisableCollection(); Server().ProcessOwnerProof();
    assert(!Server().HasPendingOwnerProof() && Server().Snapshot().entries.empty());
}
#ifdef AUDREAM_TEST_OWNER_CRYPTO
std::string ReadOwnerFixture(const std::string& root, const char* name) {
    std::ifstream input(root + "/" + name, std::ios::binary); assert(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void TestActualServerOwnerProof(const std::string& root) {
    bound_device = ReadOwnerFixture(root, "device.txt");
    bound_credential = ReadOwnerFixture(root, "credential.txt");
    const auto binary = ReadOwnerFixture(root, "proof-2048.bin");
    const Bytes proof(binary.begin(), binary.end());
    random_calls = 0x11; const auto request = AndroidPaired();
    SendProof(proof); assert(Server().HasPendingOwnerProof());
    ServerRead(102); assert(server_responses.back().second[1] == 0);
    Server().ProcessOwnerProof();
    ServerRead(102); assert(server_responses.back().second[1] == 1);
    const auto peer_binary = ReadOwnerFixture(root, "peer-2048.bin");
    assert(Bytes(peer_binary.begin(), peer_binary.end()) == Bytes(server_responses.back().second.begin() + 42, server_responses.back().second.end()));
    ServerWrite(104, AndroidFrame()); AckConfirmation(); assert(Server().Snapshot().entries.size() == 1);
    const auto local_session = Server().Snapshot().session;
    ServerWrite(102, {2,7,0,0,0,0,0,0,0});
    assert(Server().PendingOwnerAuthorization()->challenge != request.challenge);
    const auto renewed_binary = ReadOwnerFixture(root, "renewed.bin");
    SendProof(Bytes(renewed_binary.begin(), renewed_binary.end())); Server().ProcessOwnerProof();
    ServerRead(102); assert(server_responses.back().second[1] == 1);
    const auto renewed_peer = ReadOwnerFixture(root, "peer-renewed.bin");
    assert(Bytes(renewed_peer.begin(), renewed_peer.end()) == Bytes(server_responses.back().second.begin() + 42, server_responses.back().second.end()));
    assert(Server().Snapshot().session == local_session && Server().Snapshot().entries.size() == 1);
    // A real, previously accepted signature does not authorize another challenge.
    ServerWrite(102, {2,7,0,0,0,0,0,0,0});
    SendProof(proof); Server().ProcessOwnerProof();
    assert(Called("disconnect") && Server().Snapshot().entries.empty());
    random_calls = 0x11; AndroidPaired(); bound_device[0] = bound_device[0] == '1' ? '2' : '1';
    SendProof(proof); Server().ProcessOwnerProof(); assert(Called("disconnect"));
    bound_device = ReadOwnerFixture(root, "device.txt");
    random_calls = 0x11; AndroidPaired(); SendProof(proof); binding_generation = 0;
    Server().ProcessOwnerProof(); assert(Called("disconnect"));
    random_calls = 0x11; AndroidPaired(); SendProof(proof); clock_us += 5000000;
    Server().ProcessOwnerProof(); assert(Called("disconnect"));
    std::puts("Actual API proof admitted by native GATT application path; replay, wrong device and removal rejected");
}
#endif
}

extern "C" int64_t esp_timer_get_time() { return clock_us; }
// NVS is a separate native boundary; binding tests exercise its real storage
// fencing. The crypto contract build links the actual Mbed TLS verifier below.
namespace watch_voice {
uint64_t ActiveBindingGeneration() { return binding_generation; }
bool WithBoundIdentity(const std::function<bool(std::string_view, uint64_t)>& verifier) {
    return binding_generation && verifier(bound_device, binding_generation);
}
bool WithBoundDevice(const std::function<bool(const Binding&)>& verifier) {
    return binding_generation && verifier(Binding{binding_generation, bound_device, bound_credential});
}
}
#ifndef AUDREAM_TEST_OWNER_CRYPTO
namespace watch_notification {
std::optional<uint32_t> VerifyOwnerGrant(std::string_view, std::string_view, uint64_t,
    const std::array<uint8_t, 32>&, const uint8_t*, size_t) { return std::nullopt; }
std::optional<std::array<uint8_t, 32>> BuildPeerProof(std::string_view,
    const std::array<uint8_t, 32>&, const uint8_t*, size_t) { return std::nullopt; }
}
#endif
extern "C" void esp_fill_random(void* buffer, size_t length) { assert(length == 32); std::memset(buffer, ++random_calls, length); }
esp_err_t esp_ble_gatts_register_callback(esp_gatts_cb_t value) { server_callback = value; return Call("server_callback"); }
esp_err_t esp_ble_gatts_app_register(uint16_t app) { assert(app == 0x2da2 || app == 0x2da3); return Call("server_register"); }
esp_err_t esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t* db, esp_gatt_if_t interface, uint16_t count, uint8_t instance) {
    assert(instance == 0);
    if (interface == kPairingInterface) {
        assert(count == 7);
        std::copy_n(db[0].att_desc.value, 16, pairing_uuid.begin());
        assert(db[2].att_desc.perm == (ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE));
        assert(db[4].att_desc.perm == ESP_GATT_PERM_WRITE_ENC_MITM);
        assert(db[6].att_desc.perm == ESP_GATT_PERM_READ_ENC_MITM);
        return Call("pairing_attributes");
    }
    assert(interface == kServerInterface && count == 10);
    std::copy_n(db[0].att_desc.value, 16, server_uuid.begin());
    assert(db[2].att_desc.perm == (ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE));
    assert(db[4].att_desc.perm == ESP_GATT_PERM_WRITE_ENC_MITM);
    assert(db[7].att_desc.perm == (ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM));
    assert(db[4].attr_control.auto_rsp == ESP_GATT_RSP_BY_APP && db[4].att_desc.max_length == 512);
    assert(db[9].att_desc.perm == ESP_GATT_PERM_WRITE_ENC_MITM && db[9].attr_control.auto_rsp == ESP_GATT_RSP_BY_APP);
    return Call("server_attributes");
}
esp_err_t esp_ble_gatts_start_service(uint16_t handle) { assert(handle == 100 || handle == 200); return Call("server_start"); }
esp_err_t esp_ble_gatts_send_response(esp_gatt_if_t interface, uint16_t conn, uint32_t, esp_gatt_status_t status, esp_gatt_rsp_t* response) {
    assert((interface == kServerInterface || interface == kPairingInterface) && conn == connection && response && response->attr_value.len <= 74);
    auto& responses = interface == kPairingInterface ? pairing_responses : server_responses;
    responses.emplace_back(status, Bytes(response->attr_value.value, response->attr_value.value + response->attr_value.len));
    return Call("server_response");
}
esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t interface, uint16_t conn, uint16_t handle, uint16_t length, uint8_t* bytes, bool confirm) {
    assert(interface == kServerInterface && conn == connection && handle == 106 && length == 37 && confirm);
    server_indications.emplace_back(bytes, bytes + length); return Call("server_indicate");
}
esp_err_t esp_ble_gatt_set_local_mtu(uint16_t mtu) { assert(mtu == 512); return Call("local_mtu"); }
esp_err_t esp_ble_gattc_send_mtu_req(esp_gatt_if_t, uint16_t) { return Call("mtu_request"); }
esp_err_t esp_ble_gattc_register_callback(esp_gattc_cb_t value) { callback = value; return Call("register_callback"); }
esp_err_t esp_ble_gattc_app_register(uint16_t app) { assert(app == 0x2da1); return Call("register_app"); }
esp_err_t esp_ble_gap_set_security_param(esp_ble_sm_param_t kind, void* value, uint8_t size) {
    assert(size == 1);
    if (kind == ESP_BLE_SM_AUTHEN_REQ_MODE) assert(*static_cast<uint8_t*>(value) == ESP_LE_AUTH_REQ_SC_MITM_BOND);
    if (kind == ESP_BLE_SM_IOCAP_MODE) assert(*static_cast<uint8_t*>(value) == ESP_IO_CAP_IO);
    return Call("security_config");
}
esp_err_t esp_ble_gap_security_rsp(esp_bd_addr_t, bool accept) { security_replies.push_back(accept); return Call("security_reply"); }
esp_err_t esp_ble_confirm_reply(esp_bd_addr_t, bool accept) { confirmations.push_back(accept); return Call("confirm"); }
esp_err_t esp_ble_passkey_reply(esp_bd_addr_t, bool accept, uint32_t) { assert(!accept); return Call("passkey_reject"); }
esp_err_t esp_ble_gap_disconnect(esp_bd_addr_t address) { assert(!std::memcmp(address, peer, 6)); return Call("disconnect"); }
esp_err_t esp_ble_remove_bond_device(esp_bd_addr_t address) {
    std::array<uint8_t, 6> value{};
    std::copy_n(address, value.size(), value.begin());
    removed_bond_addresses.push_back(value);
    return Call("remove_bond");
}
esp_err_t esp_ble_gattc_cache_refresh(esp_bd_addr_t address) { assert(!std::memcmp(address, peer, 6)); return Call("cache_refresh"); }
esp_err_t esp_ble_set_encryption(esp_bd_addr_t, esp_ble_sec_act_t mode) { assert(mode == ESP_BLE_SEC_ENCRYPT_MITM); return Call("encrypt"); }
esp_err_t esp_ble_gattc_open(esp_gatt_if_t interface, esp_bd_addr_t address, esp_ble_addr_type_t type, bool direct) {
    assert(interface == kInterface && !std::memcmp(address, peer, 6) && type == BLE_ADDR_TYPE_RANDOM && direct);
    return Call("open");
}
esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t, uint16_t, esp_bt_uuid_t* filter) { assert(!filter); return Call("search"); }
esp_gatt_status_t esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t, uint16_t, uint16_t, uint16_t, esp_bt_uuid_t uuid,
        esp_gattc_char_elem_t* values, uint16_t* count) {
    assert(*count == 2); *count = 1;
    if (uuid.len == ESP_UUID_LEN_16) { assert(uuid.uuid.uuid16 == 0x2a05); values[0].char_handle = 4; values[0].properties = ESP_GATT_CHAR_PROP_BIT_INDICATE; }
    else if (uuid.uuid.uuid128[0] == 0xfb) { values[0].char_handle = 20; values[0].properties = ESP_GATT_CHAR_PROP_BIT_NOTIFY; }
    else if (uuid.uuid.uuid128[0] == 0xbd) { values[0].char_handle = 24; values[0].properties = ESP_GATT_CHAR_PROP_BIT_NOTIFY; }
    else { assert(uuid.uuid.uuid128[0] == 0xd9); if (missing_control) *count = 0; values[0].char_handle = 30; values[0].properties = ESP_GATT_CHAR_PROP_BIT_WRITE; }
    return ESP_GATT_OK;
}
esp_gatt_status_t esp_ble_gattc_get_descr_by_char_handle(esp_gatt_if_t, uint16_t, uint16_t handle, esp_bt_uuid_t uuid,
        esp_gattc_descr_elem_t* values, uint16_t* count) {
    assert(*count == 2 && uuid.len == ESP_UUID_LEN_16 && uuid.uuid.uuid16 == 0x2902);
    *count = 1; values[0].handle = handle + 1;
    return Call("get_descriptor") == ESP_OK ? ESP_GATT_OK : ESP_GATT_ERROR;
}
esp_err_t esp_ble_gattc_register_for_notify(esp_gatt_if_t, esp_bd_addr_t, uint16_t handle) {
    registrations.push_back(handle); return Call("register_notify");
}
esp_err_t esp_ble_gattc_unregister_for_notify(esp_gatt_if_t, esp_bd_addr_t address, uint16_t handle) {
    assert(!std::memcmp(address, peer, 6)); unregistrations.push_back(handle); return Call("unregister_notify");
}
esp_err_t esp_ble_gattc_write_char_descr(esp_gatt_if_t, uint16_t, uint16_t handle, uint16_t length, uint8_t* value,
        esp_gatt_write_type_t type, esp_gatt_auth_req_t auth) {
    assert(type == ESP_GATT_WRITE_TYPE_RSP && auth == ESP_GATT_AUTH_REQ_MITM && length == 2);
    descriptor_writes.emplace_back(handle, Bytes(value, value + length)); return Call("write_descriptor");
}
esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t, uint16_t, uint16_t handle, uint16_t length, uint8_t* value,
        esp_gatt_write_type_t type, esp_gatt_auth_req_t auth) {
    assert(type == ESP_GATT_WRITE_TYPE_RSP && auth == ESP_GATT_AUTH_REQ_MITM && handle == 30);
    control_writes.emplace_back(value, value + length); return Call("write_char");
}
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
namespace {
watch_control::BondFingerprint CurrentFingerprint() {
    watch_control::BondFingerprint fingerprint{};
    assert(watch_control::FindNativeBond(peer, fingerprint) == watch_control::BondStatus::kFound);
    return fingerprint;
}
uint64_t ApproveLocalBond(bool finalize = true) {
    bond_fixture::Reset(peer);
    const auto window = StartLocalControlPairing();
    ControlNumber(window); Auth();
    assert(!Client().ControlPeerLease(connection, peer));
    assert(Client().ControlPairingWindowCurrent(window));
    Client().Poll();
    assert(Client().ControlPairingLease(window) && Client().ControlPeerLease(connection, peer));
    assert(watch_control::ReadApproval(CurrentFingerprint()) == watch_control::BondStatus::kApproved);
    if (finalize) Client().CloseDevicePairingWindow(window);
    return window;
}
void BeginRestore() {
    Disconnected(); ++connection; Connected(); calls.clear();
    Client().Poll();
    assert(Called("encrypt") && !Client().ControlPeerLease(connection, peer));
    assert(Client().Snapshot().state == WatchAncsLinkState::kPairing);
}
void TestStoredControlBondRestoration() {
    ApproveLocalBond();
    const auto old = Client().ControlPeerLease(connection, peer);
    BeginRestore();
    Client().DisableCollection(); Client().SetEnabled(false);
    assert(!Called("disconnect"));
    Auth(); assert(!Client().ControlPeerLease(connection, peer));
    Client().Poll();
    const auto restored = Client().ControlPeerLease(connection, peer);
    assert(restored && restored != old && Client().Snapshot().state == WatchAncsLinkState::kControlReady);
    assert(!Client().Snapshot().enabled && !Called("search") && registrations.empty());
    assert(!Client().DevicePairingPeerLease(connection, peer));

    // A phone may restore encryption before the application's first poll.
    Disconnected(); ++connection; Connected(); calls.clear();
    Auth(); assert(!Client().ControlPeerLease(connection, peer));
    Client().Poll(); assert(!Client().ControlPeerLease(connection, peer));
    Client().Poll(); assert(Client().ControlPeerLease(connection, peer) && !Called("encrypt"));

    // Simulate native host shutdown/restart while keeping committed NVS/bonds.
    Reset(false); calls.clear(); Client().Poll();
    assert(Called("encrypt") && !Client().ControlPeerLease(connection, peer));
    Auth(); Client().Poll(); assert(Client().ControlPeerLease(connection, peer));
}
void TestStoredControlBondDenials() {
    ApproveLocalBond(); BeginRestore();
    Auth(true, ESP_LE_AUTH_REQ_SC_BOND); Client().Poll();
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
    ApproveLocalBond(); BeginRestore(); confirmations.clear();
    esp_ble_gap_cb_param_t gap{};
    std::copy_n(peer, 6, gap.ble_security.key_notif.bd_addr);
    gap.ble_security.key_notif.passkey = 42;
    Client().GapEvent(ESP_GAP_BLE_NC_REQ_EVT, &gap);
    assert(confirmations == std::vector<bool>{false} && Called("disconnect"));
    Auth(); Client().Poll(); assert(!Client().ControlPeerLease(connection, peer));
    ApproveLocalBond(); BeginRestore();
    gap = {}; std::copy_n(peer, 6, gap.ble_security.ble_key.bd_addr);
    Client().GapEvent(ESP_GAP_BLE_KEY_EVT, &gap);
    Auth(); Client().Poll(); assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));

    ApproveLocalBond(); BeginRestore();
    bond_fixture::bonds[0].bond_key.penc_key.ltk[0] ^= 1;
    Auth(); Client().Poll(); assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
    ApproveLocalBond(); Disconnected(); ++connection;
    bond_fixture::bonds[0].bond_key.penc_key.ltk[0] ^= 1;
    Connected(); calls.clear(); Client().Poll();
    assert(!Called("encrypt") && !Client().ControlPeerLease(connection, peer));
    ApproveLocalBond(); BeginRestore();
    clock_us += 60000001; Client().Poll();
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));
}
void TestStoredControlBondCancellationAndStaleWork() {
    auto window = ApproveLocalBond(false);
    auto fingerprint = CurrentFingerprint();
    Client().CancelControlPairing(window);
    assert(watch_control::ReadApproval(fingerprint) == watch_control::BondStatus::kRemoved);
    assert(!Client().ControlPeerLease(connection, peer));

    bond_fixture::Reset(peer);
    window = StartLocalControlPairing(); ControlNumber(window); Auth();
    fingerprint = CurrentFingerprint();
    bond_fixture::on_commit = [window] { Client().CancelControlPairing(window); };
    Client().Poll();
    assert(watch_control::ReadApproval(fingerprint) == watch_control::BondStatus::kRemoved);
    assert(!Client().ControlPeerLease(connection, peer) && !bond_fixture::handles);

    bond_fixture::Reset(peer);
    window = StartLocalControlPairing(); ControlNumber(window); Auth();
    fingerprint = CurrentFingerprint();
    bond_fixture::on_commit = [] { bond_fixture::bonds[0].bond_key.penc_key.ltk[0] ^= 1; };
    Client().Poll();
    assert(watch_control::ReadApproval(fingerprint) == watch_control::BondStatus::kRemoved);
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));

    bond_fixture::Reset(peer);
    window = StartLocalControlPairing(); ControlNumber(window); Auth();
    fingerprint = CurrentFingerprint();
    bond_fixture::on_commit = [] { clock_us += 120000001; };
    Client().Poll();
    assert(watch_control::ReadApproval(fingerprint) == watch_control::BondStatus::kRemoved);
    assert(!Client().ControlPeerLease(connection, peer) && Called("disconnect"));

    ApproveLocalBond(); Disconnected(); ++connection; Connected(); calls.clear();
    bond_fixture::on_list = [] { Disconnected(); ++connection; Connected(); };
    Client().Poll(); assert(!Called("encrypt") && !Client().ControlPeerLease(connection, peer));
    Client().Poll(); assert(Called("encrypt"));
    Auth();
    bond_fixture::on_list = [] { Disconnected(); ++connection; Connected(); };
    Client().Poll(); assert(!Client().ControlPeerLease(connection, peer));
}
void TestStoredControlBondWriteFailure() {
    bond_fixture::Reset(peer);
    const auto window = StartLocalControlPairing(); ControlNumber(window); Auth();
    bond_fixture::fail_commit = true;
    Client().Poll();
    assert(!Client().ControlPeerLease(connection, peer) && !Client().ControlPairingLease(window));
    assert(Called("disconnect") && !bond_fixture::handles);
    bond_fixture::fail_commit = false;
    Disconnected(); ++connection; Connected(); calls.clear(); Client().Poll();
    assert(!Called("encrypt") && !Client().ControlPeerLease(connection, peer));
}
void TestStoredControlBondRemoval() {
    ApproveLocalBond();
    auto fingerprint = CurrentFingerprint();
    esp_ble_gap_cb_param_t gap{};
    gap.remove_bond_dev_cmpl.status = ESP_BT_STATUS_SUCCESS;
    std::copy_n(peer, 6, gap.remove_bond_dev_cmpl.bd_addr);
    Client().GapEvent(ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT, &gap);
    assert(!Client().ControlPeerLease(connection, peer));
    Client().Poll();
    assert(watch_control::ReadApproval(fingerprint) == watch_control::BondStatus::kRemoved);
    Reset(false); calls.clear(); Client().Poll();
    assert(!Called("encrypt") && !Client().ControlPeerLease(connection, peer));

    ApproveLocalBond(); fingerprint = CurrentFingerprint(); Disconnected();
    gap = {}; gap.clear_bond_dev_cmpl.status = ESP_BT_STATUS_SUCCESS;
    Client().GapEvent(ESP_GAP_BLE_CLEAR_BOND_DEV_COMPLETE_EVT, &gap);
    Client().Poll();
    assert(watch_control::ReadApproval(fingerprint) == watch_control::BondStatus::kAbsent);
    Reset(false); calls.clear(); Client().Poll();
    assert(!Called("encrypt") && !Client().ControlPeerLease(connection, peer));
}
void TestStoredControlBondClearFailure() {
    ApproveLocalBond();
    esp_ble_gap_cb_param_t gap{}; gap.clear_bond_dev_cmpl.status = ESP_BT_STATUS_SUCCESS;
    Client().GapEvent(ESP_GAP_BLE_CLEAR_BOND_DEV_COMPLETE_EVT, &gap);
    bond_fixture::fail_commit = true; Client().Poll();
    assert(!Client().ControlPeerLease(connection, peer));
    assert(watch_control::ReadApproval(CurrentFingerprint()) == watch_control::BondStatus::kStorageError);
    bond_fixture::fail_commit = false;
    Reset(false); calls.clear(); Client().Poll();
    assert(!Called("encrypt") && !Client().ControlPeerLease(connection, peer));
}
void TestSavedControlBondRemoval(const std::string& scenario) {
    using Phase = WatchControlRemovalPhase;
    using Status = watch_control::BondStatus;
    ApproveLocalBond();
    const auto first_fingerprint = CurrentFingerprint();
    auto second = bond_fixture::bonds.front();
    second.bd_addr[0] = second.bond_key.pid_key.static_addr[0] = 8;
    second.bond_key.penc_key.ltk[0] ^= 1;
    bond_fixture::bonds.push_back(second);
    watch_control::BondFingerprint second_fingerprint{};
    assert(watch_control::FindNativeBond(second.bd_addr, second_fingerprint) == Status::kFound);
    assert(watch_control::Approve(second_fingerprint, [] { return true; }) == Status::kApproved);
    std::vector<WatchSavedControlPeer> choices;
    assert(!Client().InspectSavedControlPeers(choices) && choices.empty());
    Disconnected();
    if (scenario == "saved-empty") {
        bond_fixture::bonds.clear();
        assert(Client().InspectSavedControlPeers(choices) && choices.empty());
        assert(!Client().ForgetSavedControlPeer(1));
        return;
    }
    if (scenario == "saved-unapproved") {
        assert(watch_control::Revoke(second_fingerprint) == Status::kRemoved);
        assert(Client().InspectSavedControlPeers(choices) && choices.size() == 1);
        return;
    }
    if (scenario == "saved-malformed") {
        bond_fixture::bonds[1].bond_key.penc_key.key_size = 7;
        assert(!Client().InspectSavedControlPeers(choices) && choices.empty());
        return;
    }
    if (scenario == "saved-list-race") {
        bond_fixture::on_list = [] { bond_fixture::bonds.pop_back(); };
        assert(!Client().InspectSavedControlPeers(choices) && choices.empty());
        return;
    }
    if (scenario == "saved-lifecycle-race") {
        bond_fixture::on_list = [] { Connected(); Disconnected(); };
        assert(!Client().InspectSavedControlPeers(choices) && choices.empty());
        return;
    }
    if (scenario == "saved-poll-race") {
        bond_fixture::on_list = [] {
            Connected(); calls.clear(); Client().Poll();
            assert(!Called("encrypt"));
            Disconnected();
        };
        assert(!Client().InspectSavedControlPeers(choices) && choices.empty());
        return;
    }
    if (scenario == "saved-inspection-busy") {
        bond_fixture::on_list = [] {
            std::vector<WatchSavedControlPeer> nested;
            assert(!Client().InspectSavedControlPeers(nested));
            assert(!Client().CanOpenPairingWindow(true) && !Client().CanOpenPairingWindow(false));
        };
        assert(Client().InspectSavedControlPeers(choices) && choices.size() == 2);
        return;
    }
    assert(Client().InspectSavedControlPeers(choices) && choices.size() == 2);
    const uint64_t selected = choices.front().selection;
    assert(selected && selected != choices.back().selection);
    assert(Client().CanForgetSavedControlPeer(selected));
    assert(!Client().ForgetSavedControlPeer(0));
    if (scenario == "saved-refresh") {
        assert(Client().InspectSavedControlPeers(choices));
        assert(!Client().ForgetSavedControlPeer(selected));
        assert(choices.front().selection != selected);
        return;
    }
    if (scenario == "saved-expired" || scenario == "saved-clock-rollback") {
        clock_us = scenario == "saved-expired" ? clock_us + 120000000 : 0;
        assert(!Client().CanForgetSavedControlPeer(selected) && !Client().ForgetSavedControlPeer(selected));
        return;
    }
    if (scenario == "saved-reconnect") {
        Connected(); Disconnected();
        assert(!Client().ForgetSavedControlPeer(selected));
        return;
    }
    if (scenario == "saved-host-stop") {
        Client().HostStopping();
        assert(!Client().ForgetSavedControlPeer(selected));
        return;
    }
    if (scenario == "saved-notifications") {
        assert(Client().SetEnabled(true));
        assert(!Client().ForgetSavedControlPeer(selected));
        return;
    }
    calls.clear(); removed_bond_addresses.clear();
    assert(Client().ForgetSavedControlPeer(selected));
    assert(!Client().ForgetSavedControlPeer(selected));
    assert(!Client().CanOpenPairingWindow(true));
    assert(!Client().InspectSavedControlPeers(choices));
    if (scenario == "saved-changed-bond") bond_fixture::bonds[0].bond_key.penc_key.ltk[0] ^= 1;
    if (scenario == "saved-storage-failure") bond_fixture::fail_commit = true;
    Client().Poll();
    if (scenario == "saved-changed-bond" || scenario == "saved-storage-failure") {
        assert(Client().ControlRemovalSnapshot().phase == Phase::kFailed && removed_bond_addresses.empty());
        return;
    }
    assert(Client().ControlRemovalSnapshot().phase == Phase::kRemovingNative);
    assert(removed_bond_addresses.size() == 1);
    assert(watch_control::ReadApproval(first_fingerprint) == Status::kRemoved);
    assert(watch_control::ReadApproval(second_fingerprint) == Status::kApproved);
    esp_ble_gap_cb_param_t gap{};
    std::copy(removed_bond_addresses[0].begin(), removed_bond_addresses[0].end(), gap.remove_bond_dev_cmpl.bd_addr);
    gap.remove_bond_dev_cmpl.status = ESP_BT_STATUS_SUCCESS;
    bond_fixture::bonds.erase(bond_fixture::bonds.begin());
    Client().GapEvent(ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT, &gap);
    Client().Poll();
    assert(Client().ControlRemovalSnapshot().phase == Phase::kComplete);
    assert(Client().ControlRemovalSnapshot().peer_generation == selected);
    assert(Client().InspectSavedControlPeers(choices) && choices.size() == 1);
    assert(choices.front().stored_address[0] == 8);
    assert(watch_control::ReadApproval(second_fingerprint) == Status::kApproved);
    if (scenario == "saved-all") {
        const auto remaining = choices.front().selection;
        assert(Client().ForgetSavedControlPeer(remaining));
        Client().Poll();
        assert(Client().ControlRemovalSnapshot().phase == Phase::kRemovingNative);
        assert(removed_bond_addresses.size() == 2);
        gap = {};
        std::copy(removed_bond_addresses[1].begin(), removed_bond_addresses[1].end(), gap.remove_bond_dev_cmpl.bd_addr);
        gap.remove_bond_dev_cmpl.status = ESP_BT_STATUS_SUCCESS;
        bond_fixture::bonds.clear();
        Client().GapEvent(ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT, &gap);
        Client().Poll();
        assert(Client().ControlRemovalSnapshot().phase == Phase::kComplete);
        assert(Client().ControlRemovalSnapshot().peer_generation == remaining);
        assert(Client().InspectSavedControlPeers(choices) && choices.empty());
        assert(watch_control::ReadApproval(second_fingerprint) == Status::kRemoved);
    }
}
void TestLocalControlBondRemoval(const std::string& scenario) {
    using Phase = WatchControlRemovalPhase;
    using Error = WatchControlRemovalError;
    using Status = watch_control::BondStatus;
    if (scenario == "forget-unapproved") {
        Reset(false);
        assert(!Client().CanForgetControlPeer(Client().Snapshot().link_generation));
        assert(!Client().ForgetControlPeer(Client().Snapshot().link_generation));
        assert(Client().ControlRemovalSnapshot().phase == Phase::kIdle);
        return;
    }
    const auto window = ApproveLocalBond(scenario != "forget-provisional");
    const auto generation = Client().ControlPeerLease(connection, peer);
    const auto fingerprint = CurrentFingerprint();
    watch_control::BondFingerprint other_fingerprint{};
    if (scenario == "forget-other-bond") {
        auto other = bond_fixture::bonds.front();
        other.bd_addr[0] = other.bond_key.pid_key.static_addr[0] = 7;
        other.bond_key.penc_key.ltk[0] ^= 1;
        bond_fixture::bonds.push_back(other);
        assert(watch_control::FindNativeBond(other.bd_addr, other_fingerprint) == Status::kFound);
        assert(watch_control::Approve(other_fingerprint, [] { return true; }) == Status::kApproved);
    }
    if (scenario == "forget-provisional") {
        assert(!Client().CanForgetControlPeer(generation) && !Client().ForgetControlPeer(generation));
        Client().CloseDevicePairingWindow(window);
    }
    assert(Client().CanForgetControlPeer(generation));
    assert(!Client().ForgetControlPeer(0) && !Client().ForgetControlPeer(generation + 1));
    assert(Client().ControlPeerLease(connection, peer) == generation);
    // Native storage uses a different address from the current identity address.
    bond_fixture::bonds[0].bd_addr[0] = 9;
    assert(CurrentFingerprint() == fingerprint);
    calls.clear(); removed_bond_addresses.clear();
    BlufiControlSession channel(true);
    const auto authorize = [](uint16_t connection, const uint8_t* address) { return Client().ControlPeerLease(connection, address); };
    channel.Connected(connection, peer);
    const auto ticket = channel.Capture(authorize);
    assert(ticket);
    assert(Client().ForgetControlPeer(generation));
    assert(!Client().ForgetControlPeer(generation));
    assert(!Client().ControlPeerLease(connection, peer) && !channel.IsCurrent(ticket, authorize));
    assert(!Client().CanOpenPairingWindow(true) && !Client().CanOpenPairingWindow(false));
    assert(Called("disconnect") && !Called("remove_bond"));
    assert(Client().ControlRemovalSnapshot().phase == Phase::kRevoking);
    if (scenario == "forget-storage-failure") bond_fixture::fail_commit = true;
    if (scenario == "forget-api-failure") fail_api = "remove_bond";
    if (scenario == "forget-changed-bond") bond_fixture::bonds[0].bond_key.penc_key.ltk[0] ^= 1;
    if (scenario == "forget-list-race") bond_fixture::on_list = [] { bond_fixture::bonds.push_back(bond_fixture::bonds.front()); };
    if (scenario == "forget-absent") bond_fixture::bonds.clear();
    if (scenario == "forget-host-before-poll") Client().HostStopping();
    if (scenario == "forget-host-during-write") bond_fixture::on_commit = [] { Client().HostStopping(); };
    if (scenario == "forget-reconnect-during-write") bond_fixture::on_commit = [] {
        Disconnected(); ++connection; Connected(); Auth();
        assert(!Client().ControlPeerLease(connection, peer) && !Client().CanOpenPairingWindow(true));
    };
    Client().Poll();
    assert(!bond_fixture::handles);
    if (scenario == "forget-storage-failure") {
        assert(Client().ControlRemovalSnapshot().phase == Phase::kFailed);
        assert(Client().ControlRemovalSnapshot().error == Error::kStorage);
        assert(removed_bond_addresses.empty() && watch_control::ReadApproval(fingerprint) == Status::kStorageError);
        return;
    }
    assert(watch_control::ReadApproval(fingerprint) == Status::kRemoved);
    if (scenario == "forget-host-before-poll" || scenario == "forget-host-during-write") {
        assert(Client().ControlRemovalSnapshot().phase == Phase::kFailed);
        assert(Client().ControlRemovalSnapshot().error == Error::kHostStopped);
        assert(removed_bond_addresses.empty());
        return;
    }
    if (scenario == "forget-api-failure" || scenario == "forget-changed-bond" || scenario == "forget-list-race") {
        assert(Client().ControlRemovalSnapshot().phase == Phase::kFailed);
        assert(Client().ControlRemovalSnapshot().error == (scenario == "forget-api-failure" ? Error::kNative : Error::kChangedBond));
        assert(removed_bond_addresses.size() == (scenario == "forget-api-failure" ? 1U : 0U));
        return;
    }
    if (scenario == "forget-absent") {
        assert(Client().ControlRemovalSnapshot().phase == Phase::kComplete && removed_bond_addresses.empty());
        return;
    }
    assert(Client().ControlRemovalSnapshot().phase == Phase::kRemovingNative);
    assert(removed_bond_addresses.size() == 1 && removed_bond_addresses[0][0] == 9);
    esp_ble_gap_cb_param_t gap{};
    std::copy(removed_bond_addresses[0].begin(), removed_bond_addresses[0].end(), gap.remove_bond_dev_cmpl.bd_addr);
    gap.remove_bond_dev_cmpl.status = ESP_BT_STATUS_SUCCESS;
    if (scenario == "forget-timeout" || scenario == "forget-clock-rollback") {
        clock_us = scenario == "forget-timeout" ? clock_us + 10000000 : 0;
        Client().Poll();
        assert(Client().ControlRemovalSnapshot().phase == Phase::kFailed && Client().ControlRemovalSnapshot().error == Error::kTimeout);
        Disconnected(); ++connection; Connected(); Auth(); Client().Poll();
        assert(!Client().ControlPeerLease(connection, peer) && !Client().CanOpenPairingWindow(true));
        bond_fixture::bonds.clear();
        Client().GapEvent(ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT, &gap);
        Client().Poll();
        assert(Client().ControlRemovalSnapshot().phase == Phase::kFailed);
        return;
    }
    if (scenario == "forget-wrong-completion") {
        gap.remove_bond_dev_cmpl.bd_addr[0] ^= 1;
        Client().GapEvent(ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT, &gap);
        Client().Poll();
        assert(Client().ControlRemovalSnapshot().phase == Phase::kRemovingNative);
        gap.remove_bond_dev_cmpl.bd_addr[0] ^= 1;
    }
    if (scenario == "forget-callback-failure") gap.remove_bond_dev_cmpl.status = ESP_BT_STATUS_FAIL;
    else if (scenario != "forget-still-stored") bond_fixture::bonds.erase(bond_fixture::bonds.begin());
    Client().GapEvent(ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT, &gap);
    if (scenario != "forget-callback-failure") assert(Client().ControlRemovalSnapshot().phase == Phase::kVerifying);
    Client().Poll();
    const bool failed = scenario == "forget-callback-failure" || scenario == "forget-still-stored";
    assert(Client().ControlRemovalSnapshot().phase == (failed ? Phase::kFailed : Phase::kComplete));
    if (failed) assert(Client().ControlRemovalSnapshot().error == Error::kNative);
    assert(Client().ControlRemovalSnapshot().peer_generation == generation);
    assert(!Client().ControlPeerLease(connection, peer));
    if (scenario == "forget-other-bond") {
        assert(bond_fixture::bonds.size() == 1 && bond_fixture::bonds.front().bd_addr[0] == 7);
        assert(watch_control::ReadApproval(other_fingerprint) == Status::kApproved);
    }
    // A completed operation and a duplicate callback must not restore authorization.
    Client().GapEvent(ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT, &gap);
    Disconnected(); ++connection; Connected(); calls.clear(); Auth(); Client().Poll();
    assert(!Client().ControlPeerLease(connection, peer) && !Called("encrypt"));
}
}
#endif
void TestNativeHostRegistrationAdmission() {
    Reset(false); Disconnected();
    Client().HostStopping(); Client().HostStopped();
    assert(!Client().CanOpenPairingWindow(true) && !Client().CanOpenPairingWindow(false));
    assert(Client().HostStarted());
    calls.clear();
    assert(!Client().CanOpenPairingWindow(true) && !Client().OpenControlPairingWindow());
    assert(calls.empty());
    esp_ble_gattc_cb_param_t reg{};
    reg.reg.app_id = 0x2da2; reg.reg.status = ESP_GATT_OK;
    Event(ESP_GATTC_REG_EVT, reg);
    assert(!Client().CanOpenPairingWindow(true));
    reg.reg.app_id = 0x2da1; Event(ESP_GATTC_REG_EVT, reg);
    assert(Client().CanOpenPairingWindow(true) && Client().CanOpenPairingWindow(false));
    reg.reg.status = ESP_GATT_ERROR; Event(ESP_GATTC_REG_EVT, reg);
    assert(Client().CanOpenPairingWindow(true));
    auto window = Client().OpenControlPairingWindow(); assert(window);
    assert(!Client().CanOpenPairingWindow(true) && !Client().CanOpenPairingWindow(false));
    Client().CloseDevicePairingWindow(window);
    assert(Client().CanOpenPairingWindow(true));
    Client().HostStopping();
    reg.reg.status = ESP_GATT_OK; Event(ESP_GATTC_REG_EVT, reg);
    assert(!Client().CanOpenPairingWindow(true));
    Client().HostStopped();

    for (bool invalid_interface : {false, true}) {
        assert(Client().HostStarted());
        reg.reg.status = invalid_interface ? ESP_GATT_OK : ESP_GATT_ERROR;
        Client().GattEvent(ESP_GATTC_REG_EVT, invalid_interface ? ESP_GATT_IF_NONE : kInterface, &reg);
        assert(!Client().CanOpenPairingWindow(true));
        reg.reg.status = ESP_GATT_OK; Event(ESP_GATTC_REG_EVT, reg);
        Client().SetEnabled(false); Connected();
        assert(!Client().CanOpenPairingWindow(true) && !Client().OpenControlPairingWindow());
        assert(!Client().ControlPeerLease(connection, peer));
        Client().HostStopping(); Client().HostStopped();
    }
    for (bool poll : {false, true}) {
        assert(Client().HostStarted());
        clock_us += 59000000;
        Client().SetEnabled(true); Client().SetEnabled(false);
        clock_us += 1000000;
        if (poll) {
            Client().Poll();
            assert(Client().Snapshot().state == WatchAncsLinkState::kFailed);
        }
        reg.reg.status = ESP_GATT_OK; Event(ESP_GATTC_REG_EVT, reg);
        assert(!Client().CanOpenPairingWindow(true) && !Client().HostStarted());
        Client().HostStopping(); Client().HostStopped();
    }
    for (const char* api : {"register_callback", "register_app"}) {
        fail_api = api;
        assert(!Client().HostStarted());
        reg.reg.status = ESP_GATT_OK; Event(ESP_GATTC_REG_EVT, reg);
        assert(!Client().CanOpenPairingWindow(true));
        Client().HostStopping(); Client().HostStopped();
        fail_api.clear();
    }
    std::puts("Native host registration admission rejects pending, failed and duplicate completions");
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "pairing-mobile") return RunMobilePairingTransport();
    TestNativeHostRegistrationAdmission();
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (argc == 2 && std::string(argv[1]).rfind("saved-", 0) == 0) {
        TestSavedControlBondRemoval(argv[1]);
        std::cout << "PASS: native saved control peer " << argv[1] << '\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]).rfind("forget-", 0) == 0) {
        TestLocalControlBondRemoval(argv[1]);
        std::cout << "PASS: native local bond removal " << argv[1] << '\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "bonds") {
        TestStoredControlBondRestoration();
        TestStoredControlBondDenials();
        TestStoredControlBondCancellationAndStaleWork();
        TestStoredControlBondRemoval();
        TestStoredControlBondWriteFailure();
        std::puts("Five native control bond groups passed with real approval storage and SDK public types");
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "bonds-clear-failure") {
        TestStoredControlBondClearFailure();
        std::puts("Native clear-all-bonds storage failure rejected restoration");
        return 0;
    }
#endif
    TestConsentAndAuthentication();
    TestDevicePairingPurposeAndConsent();
    TestControlAuthorizationRequiresSecureLocalAdmission();
    TestOfflineControlPairingBeforeAndAfterConnect();
    TestOfflineControlPairingFailureAndCancellation();
    TestControlAuthorizationSurvivesCloudWindow();
    TestControlAuthorizationRevokedWithPhysicalSecurity();
    TestDevicePairingNotificationIndependence();
    TestDevicePairingExpiryAndRetirement();
    TestDevicePairingAuthenticationFailures();
    TestPairingGattWorkerRoundTrip();
    TestPairingGattReusesAuthorizedControlConnection();
    TestPairingGattMalformedAndExpiredData();
    TestPairingGattSharesDispatcher();
    TestEarlyNotificationsAndPrivateLifecycle();
    TestFailuresAndServiceChanges();
    TestBoundsTimeoutsAndHostRetirement();
    TestNativeApiFailures();
    TestMemoryOnlyOptOut();
    TestAndroidNativeSecurityBoundaries();
    TestAndroidApplyAcksAndLocalState();
    TestAndroidLimitsTimeoutsAndOptOut();
    TestAndroidNativeFailuresAndIsolation();
    TestAndroidOwnerLeaseAndProofBounds();
#ifdef AUDREAM_TEST_OWNER_CRYPTO
    assert(argc == 2); TestActualServerOwnerProof(argv[1]);
#else
    (void)argc; (void)argv;
#endif
    Server().HostStopping(); Server().HostStopped();
    PairingServer().HostStopping(); PairingServer().HostStopped();
    Client().HostStopping(); Client().HostStopped();
    std::puts("Six native ANCS client test groups passed against ESP-IDF public headers");
    std::puts("Five native Android GATT server test groups passed with the real peer security client");
    std::puts("Four native device pairing security test groups passed with notifications independently disabled");
    std::puts("Three native control authorization test groups passed with independent connection lifetime");
    std::puts("Two offline control pairing test groups passed before and after physical connection");
    std::puts("Four native pairing GATT test groups passed with bounded delivery, routing and control reuse");
}
