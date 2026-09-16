#include "watch_pairing_gatt_server.h"
#include "watch_ancs_client.h"
#include "watch_gatt_dispatcher.h"
#include "esp_timer.h"
#include <algorithm>

namespace {
constexpr uint16_t kAppId = 0x2da3, kMinimumMtu = 185;
enum Handle { kService, kHelloDeclaration, kHello, kInputDeclaration, kInput, kStatusDeclaration, kStatus };
uint8_t service_uuid[]{0x00,0x42,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint8_t hello_uuid[]{0x01,0x42,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint8_t input_uuid[]{0x02,0x42,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint8_t status_uuid[]{0x03,0x42,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint16_t primary = ESP_GATT_UUID_PRI_SERVICE, declaration = ESP_GATT_UUID_CHAR_DECLARE;
uint8_t hello_properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE;
uint8_t input_properties = ESP_GATT_CHAR_PROP_BIT_WRITE, status_properties = ESP_GATT_CHAR_PROP_BIT_READ;
esp_gatts_attr_db_t attributes[]{
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&primary), ESP_GATT_PERM_READ, 16, 16, service_uuid}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&declaration), ESP_GATT_PERM_READ, 1, 1, &hello_properties}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, hello_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, 46, 0, nullptr}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&declaration), ESP_GATT_PERM_READ, 1, 1, &input_properties}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, input_uuid, ESP_GATT_PERM_WRITE_ENC_MITM, 512, 0, nullptr}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&declaration), ESP_GATT_PERM_READ, 1, 1, &status_properties}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, status_uuid, ESP_GATT_PERM_READ_ENC_MITM, 74, 0, nullptr}},
};
uint64_t Now() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
void Wipe(std::string& value) {
    volatile char* bytes = value.empty() ? nullptr : &value[0];
    for (size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
    value.clear();
}
uint64_t Read(const uint8_t* bytes, size_t size) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return value;
}
void Put(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}
void Callback(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t* param) {
    WatchPairingGattServer::Get().GattEvent(event, interface, param);
}
}

WatchPairingGattServer& WatchPairingGattServer::Get() { static WatchPairingGattServer instance; return instance; }
bool WatchPairingGattServer::Peer(uint16_t connection, const uint8_t* address) const {
    return connected_ && connection == connection_ && address && std::equal(peer_.begin(), peer_.end(), address);
}
void WatchPairingGattServer::Clear() {
    window_ = opened_ms_ = lease_ = request_ = receiving_ms_ = total_ = 0;
    stage_ = Stage::kClosed; device_id_.clear(); Wipe(buffer_); proof_.fill(0);
}
void WatchPairingGattServer::Retire() {
    const auto window = window_;
    const bool selected = stage_ != Stage::kClosed && stage_ != Stage::kAwaitPeer;
    Clear();
    if (window) WatchAncsClient::Get().CloseDevicePairingWindow(window);
    // Only the selected native pairing purpose owns physical disconnection.
    // A merely discoverable service must not tear down ANCS/BLUFI connections.
    // An independently approved control connection survives cloud-window cleanup.
    const bool control_ready = connected_ && WatchAncsClient::Get().ControlPeerLease(connection_, peer_.data());
    retiring_ = retiring_ || (selected && connected_ && !control_ready);
}
bool WatchPairingGattServer::HostStarted() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (host_) return !failed_;
    host_ = true; started_ = failed_ = false; interface_ = ESP_GATT_IF_NONE; handles_.fill(0);
    if (!RegisterWatchGattCallback(kAppId, Callback) || esp_ble_gatts_app_register(kAppId) != ESP_OK) {
        failed_ = true; return false;
    }
    return true;
}
void WatchPairingGattServer::HostStopping() {
    std::lock_guard<std::mutex> lock(mutex_); Retire(); host_ = started_ = false;
}
void WatchPairingGattServer::HostStopped() {
    std::lock_guard<std::mutex> lock(mutex_); Clear();
    host_ = started_ = failed_ = connected_ = retiring_ = false;
    interface_ = ESP_GATT_IF_NONE; handles_.fill(0); mtu_ = 23;
}
bool WatchPairingGattServer::OpenLocalWindow(uint64_t window, const std::string& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!window || !host_ || !started_ || failed_ || retiring_ || window_ || device.size() != 36 ||
        !WatchAncsClient::Get().DevicePairingWindowCurrent(window)) return false;
    for (size_t i = 0; i < device.size(); ++i) {
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        if (separator ? device[i] != '-' : !((device[i] >= '0' && device[i] <= '9') || (device[i] >= 'a' && device[i] <= 'f'))) return false;
    }
    window_ = window; opened_ms_ = Now(); device_id_ = device; stage_ = Stage::kAwaitPeer;
    return true;
}
void WatchPairingGattServer::CloseLocalWindow(uint64_t window) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window && window == window_) Retire();
}
bool WatchPairingGattServer::Secure() {
    const auto lease = WatchAncsClient::Get().DevicePairingPeerLease(connection_, peer_.data());
    if (!lease || (lease_ && lease_ != lease)) return false;
    lease_ = lease; return true;
}
bool WatchPairingGattServer::Expire() {
    if (!window_) return false;
    const auto now = Now();
    if (!WatchAncsClient::Get().DevicePairingWindowCurrent(window_) || now < opened_ms_ || now - opened_ms_ >= 120000 ||
        ((stage_ == Stage::kReceivingChallenge || stage_ == Stage::kReceivingCredential) &&
         (now < receiving_ms_ || now - receiving_ms_ >= 10000)) ||
        (stage_ != Stage::kAwaitPeer && !WatchAncsClient::Get().Snapshot().device_pairing_peer) ||
        (lease_ && !Secure())) { Retire(); return true; }
    return false;
}
void WatchPairingGattServer::Poll() { std::lock_guard<std::mutex> lock(mutex_); Expire(); }
bool WatchPairingGattServer::WindowCurrent(uint64_t window) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !Expire() && window && window_ == window;
}
bool WatchPairingGattServer::RequestCurrent(const Request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !Expire() && Matches(request) && Secure() &&
        ((request.kind == Kind::kChallenge && stage_ == Stage::kChallengeProcessing) ||
         (request.kind == Kind::kCredential && stage_ == Stage::kCredentialProcessing));
}
bool WatchPairingGattServer::Matches(const Request& request) const {
    return window_ && request.window == window_ && request.peer_lease == lease_ && request.wire_request == request_;
}
std::optional<WatchPairingGattServer::Request> WatchPairingGattServer::TakePending() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Expire() || !window_ || !Secure() ||
        (stage_ != Stage::kChallengePending && stage_ != Stage::kCredentialPending)) return std::nullopt;
    const bool challenge = stage_ == Stage::kChallengePending;
    Request result{window_, lease_, request_, challenge ? Kind::kChallenge : Kind::kCredential, buffer_};
    Wipe(buffer_);
    stage_ = challenge ? Stage::kChallengeProcessing : Stage::kCredentialProcessing;
    return result;
}
bool WatchPairingGattServer::PublishProof(const Request& request, const std::array<uint8_t, 64>& proof) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Expire() || !Matches(request) || request.kind != Kind::kChallenge ||
        stage_ != Stage::kChallengeProcessing || !Secure()) return false;
    proof_ = proof; stage_ = Stage::kProofReady; return true;
}
bool WatchPairingGattServer::AdmitCredentialCommit(const Request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Now();
    if (!Matches(request) || request.kind != Kind::kCredential ||
        stage_ != Stage::kCredentialProcessing || now < opened_ms_ || now - opened_ms_ >= 120000 ||
        !host_ || !started_ || failed_ || retiring_ || !connected_ ||
        !WatchAncsClient::Get().DevicePairingCommitAllowed(window_, lease_, connection_, peer_.data())) return false;
    proof_.fill(0); stage_ = Stage::kCredentialCommitting; return true;
}
bool WatchPairingGattServer::BindingCommitted(const Request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Expire() || !Matches(request) || request.kind != Kind::kCredential ||
        stage_ != Stage::kCredentialCommitting || !Secure()) return false;
    stage_ = Stage::kBound; return true;
}
void WatchPairingGattServer::Reject(const Request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Matches(request) &&
        ((request.kind == Kind::kChallenge && stage_ == Stage::kChallengeProcessing) ||
         (request.kind == Kind::kCredential &&
          (stage_ == Stage::kCredentialProcessing || stage_ == Stage::kCredentialCommitting)))) Retire();
}
void WatchPairingGattServer::Respond(uint32_t transaction, uint16_t handle, esp_gatt_status_t status,
                                     const uint8_t* bytes, size_t size) {
    esp_gatt_rsp_t response{}; response.attr_value.handle = handle; response.attr_value.len = static_cast<uint16_t>(size);
    if (size) std::copy_n(bytes, size, response.attr_value.value);
    if (esp_ble_gatts_send_response(interface_, connection_, transaction, status, &response) != ESP_OK) Retire();
}
void WatchPairingGattServer::GattEvent(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t* p) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host_) return;
    if (event == ESP_GATTS_REG_EVT) {
        if (p->reg.app_id != kAppId) return;
        if (p->reg.status != ESP_GATT_OK || interface == ESP_GATT_IF_NONE || interface_ != ESP_GATT_IF_NONE) {
            failed_ = true; Retire(); return;
        }
        interface_ = interface;
        if (esp_ble_gatts_create_attr_tab(attributes, interface_, handles_.size(), 0) != ESP_OK) { failed_ = true; Retire(); }
        return;
    }
    if (interface_ == ESP_GATT_IF_NONE || (interface != interface_ && interface != ESP_GATT_IF_NONE)) return;
    if (event == ESP_GATTS_CREAT_ATTR_TAB_EVT) {
        const auto& tab = p->add_attr_tab;
        if (tab.status != ESP_GATT_OK || tab.svc_inst_id || tab.svc_uuid.len != ESP_UUID_LEN_128 ||
            !std::equal(service_uuid, service_uuid + 16, tab.svc_uuid.uuid.uuid128) ||
            tab.num_handle != handles_.size() || !tab.handles || handles_[0]) { failed_ = true; Retire(); return; }
        for (size_t i = 0; i < handles_.size(); ++i) {
            if (!tab.handles[i] || (i && tab.handles[i] <= tab.handles[i - 1])) { failed_ = true; Retire(); return; }
        }
        std::copy_n(tab.handles, handles_.size(), handles_.begin());
        if (esp_ble_gatts_start_service(handles_[0]) != ESP_OK) { failed_ = true; Retire(); }
        return;
    }
    if (event == ESP_GATTS_START_EVT) {
        if (p->start.service_handle != handles_[0] || p->start.status != ESP_GATT_OK) { failed_ = true; Retire(); return; }
        started_ = true; return;
    }
    if (event == ESP_GATTS_CONNECT_EVT) {
        if (connected_ || !started_ || failed_ || p->connect.link_role != 1) return;
        connected_ = true; retiring_ = false; connection_ = p->connect.conn_id; mtu_ = 23;
        std::copy_n(p->connect.remote_bda, 6, peer_.begin()); return;
    }
    if (event == ESP_GATTS_DISCONNECT_EVT) {
        if (!Peer(p->disconnect.conn_id, p->disconnect.remote_bda)) return;
        Retire(); connected_ = retiring_ = false; return;
    }
    if (event == ESP_GATTS_UNREG_EVT || event == ESP_GATTS_STOP_EVT || event == ESP_GATTS_DELETE_EVT) {
        failed_ = true; started_ = false; Retire(); return;
    }
    if (!connected_ || retiring_ || failed_ || !started_ || Expire()) return;
    if (event == ESP_GATTS_MTU_EVT && p->mtu.conn_id == connection_) {
        if (p->mtu.mtu < 23 || p->mtu.mtu > 517) { Retire(); return; }
        mtu_ = p->mtu.mtu; if (window_ && stage_ != Stage::kAwaitPeer && mtu_ < kMinimumMtu) Retire(); return;
    }
    if (event == ESP_GATTS_READ_EVT && Peer(p->read.conn_id, p->read.bda)) {
        const auto& read = p->read;
        if (!read.need_rsp) return;
        if (read.offset || read.is_long) { Respond(read.trans_id, read.handle, ESP_GATT_INVALID_OFFSET); return; }
        if (mtu_ < kMinimumMtu) { Respond(read.trans_id, read.handle, ESP_GATT_INVALID_ATTR_LEN); return; }
        if (read.handle == handles_[kHello]) {
            std::array<uint8_t, 46> hello{}; hello[0] = 1; hello[1] = window_ ? 1 : 0;
            Put(hello.data() + 2, window_);
            if (window_) std::copy(device_id_.begin(), device_id_.end(), hello.begin() + 10);
            Respond(read.trans_id, read.handle, ESP_GATT_OK, hello.data(), hello.size());
        } else if (read.handle == handles_[kStatus] && window_ && Secure()) {
            std::array<uint8_t, 74> status{}; status[0] = 1; status[1] = static_cast<uint8_t>(stage_);
            Put(status.data() + 2, request_);
            if (stage_ == Stage::kProofReady) std::copy(proof_.begin(), proof_.end(), status.begin() + 10);
            Respond(read.trans_id, read.handle, ESP_GATT_OK, status.data(), status.size());
        } else Respond(read.trans_id, read.handle, ESP_GATT_INSUF_AUTHORIZATION);
        return;
    }
    if (event == ESP_GATTS_EXEC_WRITE_EVT && Peer(p->exec_write.conn_id, p->exec_write.bda)) {
        Respond(p->exec_write.trans_id, 0, ESP_GATT_REQ_NOT_SUPPORTED); if (window_) Retire(); return;
    }
    if (event != ESP_GATTS_WRITE_EVT || !Peer(p->write.conn_id, p->write.bda)) return;
    const auto& write = p->write;
    if (!write.need_rsp) { if (window_) Retire(); return; }
    if (write.is_prep || write.offset || !write.value || !write.len || mtu_ < kMinimumMtu ||
        write.len > std::min<uint16_t>(512, mtu_ - 3)) {
        Respond(write.trans_id, write.handle, ESP_GATT_INVALID_ATTR_LEN); if (window_) Retire(); return;
    }
    if (write.handle == handles_[kHello]) {
        if (stage_ != Stage::kAwaitPeer || !window_ || write.len != 9 || write.value[0] != 1 ||
            Read(write.value + 1, 8) != window_ || !WatchAncsClient::Get().SelectDevicePairingPeer(connection_, peer_.data())) {
            Respond(write.trans_id, write.handle, ESP_GATT_INSUF_AUTHORIZATION); return;
        }
        stage_ = Stage::kAuthenticating; Respond(write.trans_id, write.handle, ESP_GATT_OK); return;
    }
    if (write.handle != handles_[kInput] || !window_ || !Secure()) {
        Respond(write.trans_id, write.handle, ESP_GATT_INSUF_AUTHORIZATION); return;
    }
    const auto* bytes = write.value;
    const bool header = write.len > 14 && bytes[0] == 1 && (bytes[1] == 1 || bytes[1] == 2);
    const bool challenge = header && bytes[1] == 1;
    const auto wire = header ? Read(bytes + 2, 8) : 0;
    const auto total = header ? Read(bytes + 10, 2) : 0;
    const auto offset = header ? Read(bytes + 12, 2) : 0;
    const auto expected = challenge ? Stage::kReceivingChallenge : Stage::kReceivingCredential;
    const bool first = (challenge && stage_ == Stage::kAuthenticating) || (!challenge && stage_ == Stage::kProofReady);
    if (!header || !wire || wire > 0x7fffffffffffffffULL || !total || total > (challenge ? 1024U : 128U) ||
        (first ? offset != 0 : stage_ != expected || total != total_ || offset != buffer_.size()) ||
        ((!challenge || !first) && wire != request_) || offset + write.len - 14 > total) {
        Respond(write.trans_id, write.handle, ESP_GATT_INVALID_ATTR_LEN); Retire(); return;
    }
    if (first) {
        Wipe(buffer_); buffer_.reserve(total); total_ = static_cast<uint16_t>(total); request_ = wire;
        receiving_ms_ = Now(); stage_ = expected; proof_.fill(0);
    }
    buffer_.append(reinterpret_cast<const char*>(bytes + 14), write.len - 14);
    if (buffer_.size() == total_) stage_ = challenge ? Stage::kChallengePending : Stage::kCredentialPending;
    Respond(write.trans_id, write.handle, ESP_GATT_OK);
}
