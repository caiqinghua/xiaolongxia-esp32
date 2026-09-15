#include "watch_android_gatt_server.h"
#include "watch_gatt_dispatcher.h"
#include "watch_ancs_client.h"
#include "watch_notification_owner_grant.h"
#include "watch_voice_binding.h"
#include "sdkconfig.h"
#include "esp_gap_ble_api.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr uint16_t kAppId = 0xada2;
constexpr uint64_t kOwnerTimeoutMs = 60000;
constexpr uint16_t kMinimumMtu = 185;
enum Handle { kService, kHelloDeclaration, kHello, kDataDeclaration, kData, kAckDeclaration, kAck, kCcc,
              kProofDeclaration, kProof };
// UUIDs in Bluetooth little-endian representation. Service ...4100, hello ...4101,
// data ...4102 and acknowledgement ...4103 share the same Audream namespace.
uint8_t service_uuid[]{0x00,0x41,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint8_t hello_uuid[]{0x01,0x41,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint8_t data_uuid[]{0x02,0x41,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint8_t ack_uuid[]{0x03,0x41,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint8_t proof_uuid[]{0x04,0x41,0x3c,0xa1,0x2c,0x42,0x71,0x93,0x4a,0x4d,0x84,0x98,0x71,0xac,0x16,0xa6};
uint16_t primary = ESP_GATT_UUID_PRI_SERVICE, declaration = ESP_GATT_UUID_CHAR_DECLARE, ccc = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
uint8_t hello_properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE;
uint8_t data_properties = ESP_GATT_CHAR_PROP_BIT_WRITE, ack_properties = ESP_GATT_CHAR_PROP_BIT_INDICATE;
esp_gatts_attr_db_t attributes[]{
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&primary), ESP_GATT_PERM_READ, 16, 16, service_uuid}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&declaration), ESP_GATT_PERM_READ, 1, 1, &hello_properties}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, hello_uuid, ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE, 74, 0, nullptr}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&declaration), ESP_GATT_PERM_READ, 1, 1, &data_properties}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, data_uuid, ESP_GATT_PERM_WRITE_ENC_MITM, 512, 0, nullptr}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&declaration), ESP_GATT_PERM_READ, 1, 1, &ack_properties}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, ack_uuid, ESP_GATT_PERM_READ_ENC_MITM, 37, 0, nullptr}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&ccc), ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM, 2, 0, nullptr}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&declaration), ESP_GATT_PERM_READ, 1, 1, &data_properties}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, proof_uuid, ESP_GATT_PERM_WRITE_ENC_MITM, 512, 0, nullptr}},
};
uint64_t Now() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
void Callback(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t* param) {
    WatchAndroidGattServer::Get().GattEvent(event, interface, param);
}
bool Equal(const WatchAndroidGattServer::OwnerRequest& a, const WatchAndroidGattServer::OwnerRequest& b) {
    return a.generation == b.generation && a.wire_session == b.wire_session && a.peer_lease == b.peer_lease &&
        a.peer == b.peer && a.challenge == b.challenge && a.binding_generation == b.binding_generation;
}
}
WatchAndroidGattServer& WatchAndroidGattServer::Get() { static WatchAndroidGattServer value; return value; }
bool WatchAndroidGattServer::Peer(uint16_t connection, const uint8_t* address) const {
    return connected_ && connection == connection_ && address && std::equal(peer_.begin(), peer_.end(), address);
}
void WatchAndroidGattServer::ResetLink() {
    engine_.EndSession(engine_session_); engine_session_ = 0;
    engine_.SetEnabled(false); engine_.SetEnabled(enabled_);
    request_.reset(); subscribed_ = ack_pending_ = false; mtu_ = 23;
    generation_ = 0; started_at_ = ack_at_ = 0;
    owner_deadline_ = binding_generation_ = proof_started_at_ = 0;
    request_verified_ = proof_complete_ = proof_processing_ = false; proof_.clear();
    peer_proof_.fill(0);
}
void WatchAndroidGattServer::Retire() {
    retire_pending_ = false; ResetLink();
    if (connected_ && !retiring_) { retiring_ = true; (void)esp_ble_gap_disconnect(peer_.data()); }
}
bool WatchAndroidGattServer::HostStarted() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (host_) return !failed_;
    host_ = true; failed_ = false; interface_ = ESP_GATT_IF_NONE; handles_.fill(0); started_ = false;
    if (!RegisterWatchGattCallback(kAppId, Callback) || esp_ble_gatts_app_register(kAppId) != ESP_OK) {
        failed_ = true; return false;
    }
    return true;
}
void WatchAndroidGattServer::HostStopping() {
    std::lock_guard<std::mutex> lock(mutex_); Retire(); host_ = false; started_ = false;
}
void WatchAndroidGattServer::HostStopped() {
    std::lock_guard<std::mutex> lock(mutex_); ResetLink();
    host_ = started_ = connected_ = retiring_ = retire_pending_ = failed_ = false;
    interface_ = ESP_GATT_IF_NONE; handles_.fill(0);
}
void WatchAndroidGattServer::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retire_pending_) Retire();
    enabled_ = enabled; engine_.SetEnabled(enabled);
    // An idle GATTS connection may belong to an iPhone using ANCS; leave it alone.
    if (!enabled && request_) Retire();
}
void WatchAndroidGattServer::DisableCollection() {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false; engine_.SetEnabled(false); engine_session_ = 0;
    retire_pending_ = request_.has_value(); request_.reset(); ack_pending_ = subscribed_ = false;
    proof_.clear(); proof_complete_ = proof_processing_ = request_verified_ = false;
    peer_proof_.fill(0);
    owner_deadline_ = binding_generation_ = 0;
}
bool WatchAndroidGattServer::Secure() {
    if (!host_ || !enabled_ || failed_ || retiring_ || !connected_ || !request_) return false;
    const auto lease = WatchAncsClient::Get().AndroidPeerLease(connection_, peer_.data());
    if (!lease || (request_->peer_lease && request_->peer_lease != lease)) return false;
    request_->peer_lease = lease; return true;
}
bool WatchAndroidGattServer::Expire() {
    if (retire_pending_) { Retire(); return true; }
    if (!request_) return false;
    const auto now = Now();
    if (!request_->binding_generation || request_->binding_generation != watch_voice::ActiveBindingGeneration()) {
        Retire(); return true;
    }
    if (!request_verified_ && (now < started_at_ || now - started_at_ >= kOwnerTimeoutMs)) { Retire(); return true; }
    if (engine_session_ && (now >= owner_deadline_ || now < started_at_ ||
        !binding_generation_ || binding_generation_ != watch_voice::ActiveBindingGeneration())) { Retire(); return true; }
    if ((!proof_.empty() || proof_processing_) && (now < proof_started_at_ ||
        now - proof_started_at_ >= WatchAndroidNotifications::kReceiveTimeoutMs)) { Retire(); return true; }
    if (request_->peer_lease && !Secure()) { Retire(); return true; }
    if (ack_pending_ && (now < ack_at_ || now - ack_at_ >= WatchAndroidNotifications::kReceiveTimeoutMs)) { Retire(); return true; }
    if (engine_session_ && (!Secure() || engine_.Poll(now) == WatchAndroidNotificationState::kFailed)) { Retire(); return true; }
    return false;
}
void WatchAndroidGattServer::Respond(uint32_t transaction, uint16_t handle, esp_gatt_status_t status,
    const uint8_t* bytes, size_t size) {
    esp_gatt_rsp_t response{}; response.attr_value.handle = handle;
    response.attr_value.len = static_cast<uint16_t>(size);
    if (size) std::copy_n(bytes, size, response.attr_value.value);
    if (esp_ble_gatts_send_response(interface_, connection_, transaction, status, &response) != ESP_OK) Retire();
}
void WatchAndroidGattServer::GattEvent(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t* p) {
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
        if (tab.status != ESP_GATT_OK || tab.svc_inst_id != 0 || tab.svc_uuid.len != ESP_UUID_LEN_128 ||
            !std::equal(service_uuid, service_uuid + 16, tab.svc_uuid.uuid.uuid128) || tab.num_handle != handles_.size() ||
            !tab.handles || handles_[0]) { failed_ = true; Retire(); return; }
        for (size_t i = 0; i < handles_.size(); ++i) {
            if (!tab.handles[i] || (i && tab.handles[i] <= tab.handles[i - 1])) { failed_ = true; Retire(); return; }
        }
        std::copy_n(tab.handles, handles_.size(), handles_.begin());
        if (esp_ble_gatts_start_service(handles_[kService]) != ESP_OK) { failed_ = true; Retire(); }
        return;
    }
    if (event == ESP_GATTS_START_EVT) {
        if (p->start.service_handle != handles_[kService] || p->start.status != ESP_GATT_OK) { failed_ = true; Retire(); return; }
        started_ = true; return;
    }
    if (event == ESP_GATTS_CONNECT_EVT) {
        if (connected_ || !started_ || failed_ || p->connect.link_role != 1) return;
        ResetLink(); connected_ = true; retiring_ = false; connection_ = p->connect.conn_id;
        std::copy_n(p->connect.remote_bda, 6, peer_.begin());
        if (serial_ == std::numeric_limits<uint64_t>::max()) { failed_ = true; Retire(); return; }
        generation_ = ++serial_; return;
    }
    if (event == ESP_GATTS_DISCONNECT_EVT) {
        if (!Peer(p->disconnect.conn_id, p->disconnect.remote_bda)) return;
        ResetLink(); connected_ = retiring_ = false; return;
    }
    if (event == ESP_GATTS_UNREG_EVT || event == ESP_GATTS_STOP_EVT || event == ESP_GATTS_DELETE_EVT) {
        failed_ = true; started_ = false; Retire(); return;
    }
    if (!connected_ || retiring_ || failed_ || !started_ || Expire()) return;
    // RESPONSE_EVT has no connection or transaction identity. Do not retag a
    // delayed response from an old link as a failure on a replacement phone.
    // Checked enqueue, indication confirmation and lease expiry own completion.
    if (event == ESP_GATTS_MTU_EVT && p->mtu.conn_id == connection_) {
        if (p->mtu.mtu < 23 || p->mtu.mtu > 517) { Retire(); return; }
        mtu_ = p->mtu.mtu;
        if (engine_session_ && mtu_ < kMinimumMtu) Retire();
        return;
    }
    if (event == ESP_GATTS_CONF_EVT && p->conf.conn_id == connection_ && p->conf.handle == handles_[kAck]) {
        if (!ack_pending_) return;
        if (p->conf.status != ESP_GATT_OK) { Retire(); return; }
        ack_pending_ = false; return;
    }
    if (event == ESP_GATTS_READ_EVT && Peer(p->read.conn_id, p->read.bda)) {
        const auto& read = p->read;
        if (!read.need_rsp) return;
        if (read.offset || read.is_long) { Respond(read.trans_id, read.handle, ESP_GATT_INVALID_OFFSET); return; }
        if (!Secure()) { Respond(read.trans_id, read.handle, ESP_GATT_INSUF_AUTHORIZATION); return; }
        if (read.handle == handles_[kHello]) {
            if (mtu_ < kMinimumMtu) { Respond(read.trans_id, read.handle, ESP_GATT_INVALID_ATTR_LEN); return; }
            std::array<uint8_t, 74> response{}; response[0] = 2; response[1] = request_verified_ ? 1 : 0;
            for (size_t i = 0; i < 8; ++i) response[2 + i] = static_cast<uint8_t>(request_->wire_session >> (8 * i));
            std::copy(request_->challenge.begin(), request_->challenge.end(), response.begin() + 10);
            std::copy(peer_proof_.begin(), peer_proof_.end(), response.begin() + 42);
            Respond(read.trans_id, read.handle, ESP_GATT_OK, response.data(), response.size());
        } else if (read.handle == handles_[kCcc]) {
            const uint8_t response[]{static_cast<uint8_t>(subscribed_ ? 2 : 0), 0};
            Respond(read.trans_id, read.handle, ESP_GATT_OK, response, 2);
        } else Respond(read.trans_id, read.handle, ESP_GATT_READ_NOT_PERMIT);
        return;
    }
    if (event == ESP_GATTS_EXEC_WRITE_EVT && Peer(p->exec_write.conn_id, p->exec_write.bda)) {
        Respond(p->exec_write.trans_id, 0, ESP_GATT_REQ_NOT_SUPPORTED); if (request_) Retire(); return;
    }
    if (event != ESP_GATTS_WRITE_EVT || !Peer(p->write.conn_id, p->write.bda)) return;
    const auto& write = p->write;
    if (!write.need_rsp) { if (request_) Retire(); return; }
    if (write.is_prep || write.offset || !write.value || !write.len || write.len > std::min<uint16_t>(512, mtu_ - 3)) {
        Respond(write.trans_id, write.handle, ESP_GATT_INVALID_ATTR_LEN); if (request_) Retire(); return;
    }
    if (write.handle == handles_[kHello]) {
        uint64_t wire = 0;
        if (write.len == 9) for (size_t i = 0; i < 8; ++i) wire |= static_cast<uint64_t>(write.value[i + 1]) << (8 * i);
        // A fresh challenge renews the owner lease without replacing private
        // notification state. The previous lease still expires on its deadline.
        if (write.len == 9 && write.value[0] == 2 && Secure() && engine_session_ &&
            request_verified_ && wire == request_->wire_session && !ack_pending_) {
            esp_fill_random(request_->challenge.data(), request_->challenge.size());
            started_at_ = Now(); request_verified_ = false;
            proof_.clear(); proof_complete_ = proof_processing_ = false;
            peer_proof_.fill(0);
            Respond(write.trans_id, write.handle, ESP_GATT_OK); return;
        }
        const auto binding = watch_voice::ActiveBindingGeneration();
        if (!enabled_ || request_ || !binding || write.len != 9 || write.value[0] != 1 || !wire || wire > 0x7fffffffffffffffULL ||
            !WatchAncsClient::Get().SelectAndroidPeer(connection_, peer_.data())) {
            Respond(write.trans_id, write.handle, ESP_GATT_INSUF_AUTHORIZATION); return;
        }
        request_ = OwnerRequest{generation_, wire, 0, peer_, {}};
        request_->binding_generation = binding;
        esp_fill_random(request_->challenge.data(), request_->challenge.size());
        started_at_ = Now(); Respond(write.trans_id, write.handle, ESP_GATT_OK); return;
    }
    if (!Secure()) { Respond(write.trans_id, write.handle, ESP_GATT_INSUF_AUTHORIZATION); if (engine_session_) Retire(); return; }
    if (write.handle == handles_[kProof]) {
        if (!subscribed_ || request_verified_ || proof_complete_ || proof_processing_ || mtu_ < kMinimumMtu) {
            Respond(write.trans_id, write.handle, ESP_GATT_INSUF_AUTHORIZATION); return;
        }
        if (proof_.empty()) proof_started_at_ = Now();
        if (proof_.size() + write.len > watch_notification::kOwnerEnvelopeMaxSize) {
            Respond(write.trans_id, write.handle, ESP_GATT_INVALID_ATTR_LEN); Retire(); return;
        }
        proof_.insert(proof_.end(), write.value, write.value + write.len);
        if (proof_.size() >= watch_notification::kPeerChallengeSize + watch_notification::kOwnerGrantPrefixSize) {
            const size_t signature_size = proof_[124] | (static_cast<size_t>(proof_[125]) << 8);
            const size_t expected = watch_notification::kPeerChallengeSize + watch_notification::kOwnerGrantPrefixSize + signature_size;
            if ((signature_size != 256 && signature_size != 384 && signature_size != 512) || proof_.size() > expected) {
                Respond(write.trans_id, write.handle, ESP_GATT_INVALID_PDU); Retire(); return;
            }
            proof_complete_ = proof_.size() == expected;
        }
        Respond(write.trans_id, write.handle, ESP_GATT_OK); return;
    }
    if (write.handle == handles_[kCcc]) {
        if (write.len != 2 || write.value[1] || (write.value[0] != 0 && write.value[0] != 2)) {
            Respond(write.trans_id, write.handle, ESP_GATT_INVALID_ATTR_LEN); return;
        }
        subscribed_ = write.value[0] == 2;
        Respond(write.trans_id, write.handle, ESP_GATT_OK);
        if (!subscribed_ && engine_session_) Retire();
        return;
    }
    if (write.handle != handles_[kData] || !engine_session_ || !subscribed_ || ack_pending_ || mtu_ < kMinimumMtu) {
        Respond(write.trans_id, write.handle, ESP_GATT_INSUF_AUTHORIZATION); return;
    }
    const auto result = engine_.Receive(engine_session_, write.value, write.len, Now());
    if (result.result != WatchAndroidNotifications::Result::kReceiving && result.result != WatchAndroidNotifications::Result::kApplied) {
        Respond(write.trans_id, write.handle, ESP_GATT_INVALID_PDU); Retire(); return;
    }
    Respond(write.trans_id, write.handle, ESP_GATT_OK);
    if (retiring_ || !request_) return;
    if (!result.acknowledgement.empty()) {
        if (!WatchAncsClient::Get().RenewAndroidPeer(request_->peer_lease)) { Retire(); return; }
        ack_pending_ = true; ack_at_ = Now();
        auto ack = result.acknowledgement;
        if (esp_ble_gatts_send_indicate(interface_, connection_, handles_[kAck], static_cast<uint16_t>(ack.size()), ack.data(), true) != ESP_OK)
            Retire();
    }
}
void WatchAndroidGattServer::Poll() { std::lock_guard<std::mutex> lock(mutex_); Expire(); }
std::optional<WatchAndroidGattServer::OwnerRequest> WatchAndroidGattServer::PendingOwnerAuthorization() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Expire() || request_verified_ || !subscribed_ || mtu_ < kMinimumMtu || !Secure()) return std::nullopt;
    return request_;
}
bool WatchAndroidGattServer::ApplyOwnerAuthorization(const OwnerRequest& verified, uint64_t binding, uint32_t lease,
    const std::array<uint8_t, 32>& peer_proof) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Expire() || request_verified_ || !subscribed_ || mtu_ < kMinimumMtu || !Secure() || !Equal(verified, *request_) ||
        !binding || binding != request_->binding_generation || binding != watch_voice::ActiveBindingGeneration() || !lease ||
        lease > watch_notification::kOwnerGrantMaxLeaseSeconds ||
        started_at_ > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(lease) * 1000 ||
        Now() >= started_at_ + static_cast<uint64_t>(lease) * 1000) return false;
    if (!engine_session_) engine_session_ = engine_.BeginAuthorizedSession(request_->wire_session, Now());
    if (!engine_session_) { Retire(); return false; }
    if (!WatchAncsClient::Get().RenewAndroidPeer(request_->peer_lease)) { Retire(); return false; }
    binding_generation_ = binding; owner_deadline_ = started_at_ + static_cast<uint64_t>(lease) * 1000;
    request_verified_ = true; proof_.clear(); proof_complete_ = proof_processing_ = false;
    peer_proof_ = peer_proof;
    return true;
}
bool WatchAndroidGattServer::HasPendingOwnerProof() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !Expire() && proof_complete_ && !proof_processing_;
}
void WatchAndroidGattServer::ProcessOwnerProof() {
    OwnerRequest captured;
    std::vector<uint8_t> proof;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Expire() || !proof_complete_ || proof_processing_ || !Secure()) return;
        captured = *request_; proof = std::move(proof_); proof_complete_ = false; proof_processing_ = true;
    }
    const bool applied = watch_voice::WithBoundDevice([&](const watch_voice::Binding& binding) {
        std::array<uint8_t, 32> nonce{};
        std::copy_n(proof.data(), nonce.size(), nonce.begin());
        const auto* signed_proof = proof.data() + nonce.size();
        const auto signed_size = proof.size() - nonce.size();
        const auto lease = watch_notification::VerifyOwnerGrant(CONFIG_AUDREAM_NOTIFICATION_SERVER_PUBLIC_KEY_B64,
            binding.device_id, captured.wire_session, captured.challenge, signed_proof, signed_size);
        if (!lease) return false;
        const auto peer_proof = watch_notification::BuildPeerProof(binding.credential, nonce, signed_proof, signed_size);
        return peer_proof && ApplyOwnerAuthorization(captured, binding.generation, *lease, *peer_proof);
    });
    if (!applied) RevokeOwnerAuthorization(captured);
}
bool WatchAndroidGattServer::RevokeOwnerAuthorization(const OwnerRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!request_ || !Equal(request, *request_)) return false;
    Retire(); return true;
}
WatchAndroidNotificationSnapshot WatchAndroidGattServer::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    Expire();
    auto snapshot = engine_.Snapshot(Now());
    if (request_ && snapshot.state == WatchAndroidNotificationState::kReady)
        snapshot.peer_lease = request_->peer_lease;
    return snapshot;
}
bool WatchAndroidGattServer::Clear(uint64_t session) {
    std::lock_guard<std::mutex> lock(mutex_); return !Expire() && engine_.Clear(session, Now());
}
bool WatchAndroidGattServer::MarkRead(uint64_t session, uint32_t uid, uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutex_); return !Expire() && engine_.MarkRead(session, uid, revision, Now());
}
