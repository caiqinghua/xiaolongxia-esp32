#include "watch/watch_pairing_controller.h"
#include "watch/watch_ancs_client.h"
#include "watch/watch_voice_binding.h"
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <nvs.h>
#include <sodium.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

namespace {
using namespace watch_voice;
using Controller = PairingController;
using Phase = Controller::Phase;
using Error = Controller::Error;
using Transport = WatchPairingGattServer;
const std::string device = "12345678-1234-4234-8234-123456789abc";
const std::string user = "22345678-1234-4234-8234-123456789abc";
const std::string challenge = "32345678-1234-4234-8234-123456789abc";
const std::string credential = "adv_" + std::string(43, 'a');
const Endpoints endpoints{"https://api.example", "wss://voice.example/v1/device"};
const auto owner = std::this_thread::get_id();
std::atomic<uint64_t> now{1000}, unix_seconds{1789401600};
std::atomic<bool> eligible{true}, reserved{false}, http_live{false};
bool acquire_ok = true, tls_ok = true, encrypted = true, identity_present = true;
bool fail_write = false, fail_commit = false, native_open_ok = true, gatt_open_ok = true;
bool native_host_ready = true;
unsigned acquired = 0, released = 0;
std::atomic<unsigned> http_requests{0}, http_cleanups{0}, writes{0}, commits{0}, identity_reads{0}, storage_opens{0};
std::vector<uint8_t> identity, disk, pending_disk;
std::function<void()> on_storage_open, on_http_cleanup, on_acquire;
std::function<bool()> reservation_probe;
bool HasReservation() { return reservation_probe ? reservation_probe() : reserved.load(); }

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool enabled = false, entered = false, proceed = false;
    void Block() { std::lock_guard<std::mutex> lock(mutex); enabled = true; }
    void Pass() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!enabled) return;
        entered = true; cv.notify_all();
        assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return proceed; }));
    }
    void Await() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
    }
    void Unblock() { std::lock_guard<std::mutex> lock(mutex); proceed = true; cv.notify_all(); }
} identity_gate, http_gate, write_gate, binding_read_gate, forget_gate, saved_read_gate;

std::mutex transport_mutex;
uint64_t next_window = 0, live_window = 0, lease = 7;
bool peer_confirmed = false;
bool local_control_mode = false;
uint64_t local_control_grant = 0;
bool forget_allowed = false;
bool saved_allowed = false, saved_read_ok = true, saved_empty = false;
unsigned saved_remaining = 2, saved_forget_fail_call = 0;
bool saved_forget_active = false;
unsigned saved_reads = 0;
WatchControlRemovalSnapshot forget_snapshot;
WatchControlRemovalError forget_error = WatchControlRemovalError::kNone;
std::atomic<unsigned> forget_calls{0};
std::atomic<bool> forget_io_pending{false};
Transport::Stage stage = Transport::Stage::kClosed;
std::optional<Transport::Request> incoming;
Transport::Request processing;
std::array<uint8_t, 64> published{};
unsigned proofs = 0, admissions = 0, bound_reports = 0;

std::string Payload(uint64_t expiry = 1789401720) {
    return "{\"domain\":\"audream-pairing-v1\",\"device_id\":\"" + device +
        "\",\"user_id\":\"" + user + "\",\"challenge_id\":\"" + challenge +
        "\",\"nonce\":\"" + std::string(43, 'x') + "\",\"expires_at\":" + std::to_string(expiry) + "}";
}
std::string receipt = "{\"device_id\":\"" + device + "\",\"user_id\":\"" + user +
    "\",\"challenge_id\":\"" + challenge + "\",\"credential_version\":1,\"status\":\"pending\"}";
std::string http_body, url;
std::map<std::string, std::string> headers;
size_t read_offset = 0;
int http_status = 200;

Controller::Hooks Hooks() {
    return {
        [] { assert(std::this_thread::get_id() == owner && !reserved); if (!acquire_ok) return false;
             reserved = true; ++acquired; if (on_acquire) on_acquire(); return true; },
        [] { assert(std::this_thread::get_id() == owner && reserved && !http_live);
             reserved = false; ++released; },
        [] { return eligible.load(); }, [] { return unix_seconds.load(); },
        [] { assert(std::this_thread::get_id() != owner && reserved); return tls_ok; }
    };
}
void Wait(const std::function<bool()>& condition) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void Reach(Controller& controller, Phase target) {
    Wait([&] { controller.Poll(); return controller.Snapshot().phase == target; });
}
void Disconnect() {
    std::lock_guard<std::mutex> lock(transport_mutex);
    live_window = 0; peer_confirmed = false; stage = Transport::Stage::kClosed; incoming.reset(); ++lease;
}
void Send(Transport::Kind kind, const std::string& payload, uint64_t wire = 19) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    assert(live_window && peer_confirmed && !incoming);
    incoming = Transport::Request{live_window, lease, wire, kind, payload};
    stage = kind == Transport::Kind::kChallenge ? Transport::Stage::kChallengePending : Transport::Stage::kCredentialPending;
}
void Ready(Controller& controller) {
    assert(controller.Open()); Reach(controller, Phase::kAwaitPeer);
    assert(!controller.ConfirmPeer(99, true));
    assert(controller.ConfirmPeer(1, true));
}
void Offered(Controller& controller, const std::string& payload = Payload()) {
    Ready(controller); Send(Transport::Kind::kChallenge, payload); Reach(controller, Phase::kAwaitApproval);
    const auto state = controller.Snapshot();
    assert(state.offer.request && state.offer.device_id == device && state.offer.user_id == user &&
           state.offer.challenge_id == challenge && proofs == 0 && http_requests == 0 && writes == 0);
}
void Signed(Controller& controller) {
    Offered(controller);
    assert(!controller.ApproveAccount(controller.Snapshot().offer.request + 1, true));
    assert(controller.ApproveAccount(controller.Snapshot().offer.request, true));
    Reach(controller, Phase::kAwaitCredential);
    assert(proofs == 1 && http_requests == 0 && writes == 0);
    const auto payload = Payload();
    assert(crypto_sign_verify_detached(published.data(), reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size(), identity.data() + 169) == 0);
}
void StartVerification(Controller& controller) {
    Signed(controller); Send(Transport::Kind::kCredential, credential); controller.Poll();
    assert(controller.Snapshot().phase == Phase::kVerifying);
}
void AssertNoBinding() {
    assert(disk.empty() && writes == 0 && commits == 0 && bound_reports == 0 && ActiveBindingGeneration() == 0);
}
void AssertSaved() {
    Binding binding;
    assert(LoadBinding(binding) == BindingStatus::kBound && binding.device_id == device &&
        binding.credential == credential && binding.generation == 1);
    assert(writes == 1 && commits == 1 && http_requests == 1 && http_cleanups == 1 && !http_live);
}
void Initialize() {
    assert(sodium_init() >= 0);
    identity.assign(233, 0);
    std::copy_n("AID1", 4, identity.begin());
    std::copy(device.begin(), device.end(), identity.begin() + 4);
    identity[40] = 8; std::copy_n("WATCH-01", 8, identity.begin() + 41);
    std::fill(identity.begin() + 201, identity.end(), 9);
    std::array<uint8_t, 64> secret{};
    assert(crypto_sign_seed_keypair(identity.data() + 169, secret.data(), identity.data() + 201) == 0);
    sodium_memzero(secret.data(), secret.size());
}
}

// Transport boundary only. Native GATT framing and BLE security are exercised
// separately by test_watch_notifications.sh against actual ESP-IDF public types.
WatchPairingGattServer& WatchPairingGattServer::Get() { static WatchPairingGattServer server; return server; }
WatchAncsClient& WatchAncsClient::Get() { static WatchAncsClient client; return client; }
bool WatchAncsClient::CanInspectSavedControlPeers() const {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return saved_allowed && native_host_ready;
}
bool WatchAncsClient::InspectSavedControlPeers(std::vector<WatchSavedControlPeer>& peers) {
    assert(HasReservation() && std::this_thread::get_id() != owner);
    saved_read_gate.Pass();
    std::lock_guard<std::mutex> lock(transport_mutex);
    ++saved_reads;
    peers.clear();
    if (!saved_allowed || !native_host_ready || !saved_read_ok) return false;
    if (!saved_empty && saved_remaining) {
        peers.push_back({lease, {1,2,3,4,5,6}});
        if (saved_remaining > 1) peers.push_back({lease + 1, {6,5,4,3,2,1}});
    }
    return true;
}
bool WatchAncsClient::CanForgetSavedControlPeer(uint64_t token) const {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return saved_allowed && native_host_ready && !saved_empty && saved_remaining &&
        (token == lease || (saved_remaining > 1 && token == lease + 1));
}
bool WatchAncsClient::ForgetSavedControlPeer(uint64_t token) {
    assert(HasReservation() && std::this_thread::get_id() != owner);
    std::lock_guard<std::mutex> lock(transport_mutex);
    if (!saved_allowed || !native_host_ready || saved_empty || !saved_remaining ||
        (token != lease && (saved_remaining <= 1 || token != lease + 1)) ||
        (saved_forget_fail_call && forget_calls + 1 == saved_forget_fail_call)) return false;
    saved_allowed = false;
    saved_forget_active = true;
    ++forget_calls;
    forget_snapshot = {token, WatchControlRemovalPhase::kRevoking, WatchControlRemovalError::kNone};
    return true;
}
bool WatchAncsClient::CanForgetControlPeer(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return generation && generation == lease && forget_allowed && native_host_ready;
}
bool WatchAncsClient::ForgetControlPeer(uint64_t generation) {
    assert(HasReservation() && std::this_thread::get_id() != owner);
    std::lock_guard<std::mutex> lock(transport_mutex);
    if (!generation || generation != lease || !forget_allowed || !native_host_ready) return false;
    forget_allowed = false;
    local_control_grant = 0;
    ++forget_calls;
    forget_snapshot = {generation, WatchControlRemovalPhase::kRevoking, WatchControlRemovalError::kNone};
    return true;
}
void WatchAncsClient::Poll() {
    assert(HasReservation() && std::this_thread::get_id() != owner);
    forget_gate.Pass();
    std::lock_guard<std::mutex> lock(transport_mutex);
    forget_snapshot.phase = forget_error == WatchControlRemovalError::kNone
        ? WatchControlRemovalPhase::kComplete : WatchControlRemovalPhase::kFailed;
    forget_snapshot.error = forget_error;
    if (saved_forget_active && forget_error == WatchControlRemovalError::kNone) {
        assert(saved_remaining);
        --saved_remaining;
        saved_allowed = true;
        saved_forget_active = false;
    }
}
WatchControlRemovalSnapshot WatchAncsClient::ControlRemovalSnapshot() const {
    std::lock_guard<std::mutex> lock(transport_mutex);
    auto result = forget_snapshot;
    result.io_pending = forget_io_pending.load();
    return result;
}
bool WatchAncsClient::CanOpenPairingWindow(bool control_only) const {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return native_host_ready && !live_window && (!control_only || !local_control_grant);
}
uint64_t WatchAncsClient::OpenDevicePairingWindow() {
    std::lock_guard<std::mutex> lock(transport_mutex);
    assert(HasReservation() && std::this_thread::get_id() == owner);
    if (!native_open_ok) return 0;
    live_window = ++next_window; peer_confirmed = false; return live_window;
}
uint64_t WatchAncsClient::OpenControlPairingWindow() {
    const auto window = OpenDevicePairingWindow();
    std::lock_guard<std::mutex> lock(transport_mutex);
    local_control_mode = window != 0;
    return window;
}
bool WatchAncsClient::ControlPairingWindowCurrent(uint64_t window) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return window && window == live_window && local_control_mode;
}
uint64_t WatchAncsClient::ControlPairingLease(uint64_t window) const {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return window && window == local_control_grant ? lease : 0;
}
void WatchAncsClient::CancelControlPairing(uint64_t window) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    if (window && window == local_control_grant) local_control_grant = 0;
    if (window && window == live_window && local_control_mode) {
        live_window = 0; local_control_mode = false; peer_confirmed = false;
    }
}
bool WatchAncsClient::ConfirmControlPairing(uint64_t window, uint64_t confirmation, bool accept) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    if (!window || window != live_window || !local_control_mode || confirmation != 1) return false;
    peer_confirmed = accept;
    if (!accept) { live_window = 0; local_control_mode = false; }
    return true;
}
void WatchAncsClient::CloseDevicePairingWindow(uint64_t window) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    if (live_window == window) { live_window = 0; peer_confirmed = false; stage = Transport::Stage::kClosed; }
}
bool WatchAncsClient::ConfirmDevicePairing(uint64_t confirmation, bool accept) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    if (!live_window || confirmation != 1) return false;
    peer_confirmed = accept; return true;
}
bool WatchPairingGattServer::OpenLocalWindow(uint64_t window, const std::string& id) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    assert(id == device && window == live_window);
    if (!gatt_open_ok) return false;
    stage = Stage::kAwaitPeer; return true;
}
void WatchPairingGattServer::CloseLocalWindow(uint64_t window) {
    WatchAncsClient::Get().CloseDevicePairingWindow(window);
    std::lock_guard<std::mutex> lock(transport_mutex); incoming.reset();
}
bool WatchPairingGattServer::WindowCurrent(uint64_t window) {
    std::lock_guard<std::mutex> lock(transport_mutex); return window && window == live_window;
}
bool WatchPairingGattServer::RequestCurrent(const Request& request) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return live_window && request.window == live_window && peer_confirmed && request.peer_lease == lease &&
        processing.wire_request == request.wire_request && processing.kind == request.kind &&
        (stage == Stage::kChallengeProcessing || stage == Stage::kCredentialProcessing);
}
std::optional<WatchPairingGattServer::Request> WatchPairingGattServer::TakePending() {
    std::lock_guard<std::mutex> lock(transport_mutex);
    if (!live_window || !peer_confirmed || !incoming) return std::nullopt;
    auto result = incoming; incoming.reset(); processing = *result;
    stage = processing.kind == Kind::kChallenge ? Stage::kChallengeProcessing : Stage::kCredentialProcessing;
    return result;
}
bool WatchPairingGattServer::PublishProof(const Request& request, const std::array<uint8_t, 64>& proof) {
    if (!RequestCurrent(request)) return false;
    std::lock_guard<std::mutex> lock(transport_mutex);
    published = proof; ++proofs; stage = Stage::kProofReady; return true;
}
bool WatchPairingGattServer::AdmitCredentialCommit(const Request& request) {
    assert(HasReservation() && !http_live && std::this_thread::get_id() != owner);
    if (!RequestCurrent(request)) return false;
    std::lock_guard<std::mutex> lock(transport_mutex);
    assert(stage == Stage::kCredentialProcessing && admissions == 0);
    ++admissions; stage = Stage::kCredentialCommitting; return true;
}
bool WatchPairingGattServer::BindingCommitted(const Request& request) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    assert(commits == 1 && !http_live && std::this_thread::get_id() == owner);
    if (!live_window || request.window != live_window || request.peer_lease != lease) return false;
    assert(stage == Stage::kCredentialCommitting);
    stage = Stage::kBound; ++bound_reports; return true;
}

int64_t esp_timer_get_time() { return static_cast<int64_t>(now.load() * 1000); }
bool esp_flash_encryption_enabled() { return encrypted; }
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    ++storage_opens;
    if (std::string(name) == "audream_ident") {
        assert(mode == NVS_READONLY && std::this_thread::get_id() != owner && HasReservation());
        if (!identity_present) return ESP_ERR_NVS_NOT_FOUND;
        *handle = 1; return ESP_OK;
    }
    assert(std::string(name) == "audream_voice");
    if (mode == NVS_READONLY && disk.empty()) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 2;
    if (mode == NVS_READWRITE && on_storage_open) on_storage_open();
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 || handle == 2); }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* data, size_t* size) {
    assert((handle == 1 && std::string(key) == "identity_v1") || (handle == 2 && std::string(key) == "binding_v1"));
    const auto& bytes = handle == 1 ? identity : disk;
    if (bytes.empty()) return ESP_ERR_NVS_NOT_FOUND;
    if (!data) { *size = bytes.size(); return ESP_OK; }
    if (*size < bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    if (handle == 1) { ++identity_reads; identity_gate.Pass(); }
    else binding_read_gate.Pass();
    std::memcpy(data, bytes.data(), bytes.size()); *size = bytes.size(); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* data, size_t size) {
    assert(handle == 2 && std::string(key) == "binding_v1" && size == 176 && !http_live);
    ++writes; write_gate.Pass();
    if (fail_write) return ESP_FAIL;
    const auto* bytes = static_cast<const uint8_t*>(data); pending_disk.assign(bytes, bytes + size); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 2); ++commits; disk = pending_disk;
    return fail_commit ? ESP_FAIL : ESP_OK;
}

struct esp_http_client {};
static esp_http_client http;
esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }
void esp_log_level_set(const char* tag, int level) { assert(std::string(tag) == "HTTP_CLIENT" && level == ESP_LOG_WARN); }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    assert(HasReservation() && !http_live && std::this_thread::get_id() != owner);
    assert(config->disable_auto_redirect && config->transport_type == HTTP_TRANSPORT_OVER_SSL &&
        config->crt_bundle_attach == esp_crt_bundle_attach && config->method == HTTP_METHOD_POST);
    url = config->url;
    assert(url == endpoints.api_origin + "/v1/devices/" + device + "/pairing/verify");
    http_live = true; ++http_requests; return &http;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t handle) {
    assert(handle == &http && http_live); http_live = false; ++http_cleanups;
    if (on_http_cleanup) on_http_cleanup();
    return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int timeout) { assert(timeout > 0 && timeout <= 5000); return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char* key, const char* value) {
    headers[key] = value; return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t, int) { return ESP_OK; }
int esp_http_client_write(esp_http_client_handle_t, const char* data, int length) {
    const auto count = std::min(length, 7); http_body.append(data, count); return count;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) {
    assert(http_body == EncodePairingVerification(user, challenge));
    assert(headers["Authorization"] == "Device " + credential && headers["Cache-Control"] == "no-store");
    http_gate.Pass(); return static_cast<int64_t>(receipt.size());
}
int esp_http_client_get_status_code(esp_http_client_handle_t) { return http_status; }
int esp_http_client_read(esp_http_client_handle_t, char* bytes, int capacity) {
    const auto count = std::min({receipt.size() - read_offset, size_t(capacity), size_t(13)});
    std::memcpy(bytes, receipt.data() + read_offset, count); read_offset += count; return static_cast<int>(count);
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) { return read_offset == receipt.size(); }

int main(int argc, char** argv) {
    assert(argc == 2); Initialize();
    const std::string scenario = argv[1];
    Controller controller(endpoints, Hooks());
    if (scenario.rfind("saved-", 0) == 0) {
        saved_allowed = true;
        saved_remaining = 2;
        saved_forget_fail_call = scenario == "saved-all-second-failed" ? 2 : 0;
        identity_present = encrypted = tls_ok = false; unix_seconds = 0;
        saved_empty = scenario == "saved-empty";
        saved_read_ok = scenario != "saved-read-failed";
        if (scenario == "saved-cancel-read" || scenario == "saved-stop-read") saved_read_gate.Block();
        assert(controller.OpenSavedControlPeers());
        assert(!controller.SelectSavedControlPeer(lease));
        assert(!controller.OpenSavedControlPeers());
        if (scenario == "saved-cancel-read" || scenario == "saved-stop-read") {
            saved_read_gate.Await();
            if (scenario == "saved-stop-read") assert(!controller.StopWithin(0));
            else controller.Cancel();
            controller.Poll(); assert(controller.Active() && HasReservation());
            saved_read_gate.Unblock(); Reach(controller, Phase::kCancelled);
        } else if (!saved_read_ok) Reach(controller, Phase::kFailed);
        else {
            Reach(controller, Phase::kAwaitSavedPeer);
            assert(controller.Snapshot().removing_control && controller.Snapshot().removing_saved_control);
            assert(controller.Snapshot().saved_peers.size() == (saved_empty ? 0 : 2));
            assert(!controller.ConfirmControlRemoval(lease, true));
            assert(!controller.SelectSavedControlPeer(0) && !controller.SelectSavedControlPeer(lease + 2));
            if (saved_empty) { controller.Cancel(); Reach(controller, Phase::kCancelled); }
            else if (scenario == "saved-stale" || scenario == "saved-expired" || scenario == "saved-clock-rollback") {
                if (scenario == "saved-stale") saved_allowed = false;
                else now = scenario == "saved-expired" ? 121000 : 999;
                assert(!controller.SelectSavedControlPeer(lease));
                Reach(controller, Phase::kFailed);
            } else if (scenario.rfind("saved-all-", 0) == 0) {
                assert(controller.SelectAllSavedControlPeers());
                assert(controller.Snapshot().phase == Phase::kAwaitAllControlRemoval);
                assert(controller.Snapshot().removing_all_control);
                assert(!controller.SelectSavedControlPeer(lease));
                const bool reject = scenario == "saved-all-reject";
                assert(controller.ConfirmAllControlRemoval(!reject));
                Reach(controller, reject ? Phase::kCancelled :
                    scenario == "saved-all-second-failed" ? Phase::kFailed : Phase::kAllControlRemoved);
                assert(forget_calls == (reject ? 0U : scenario == "saved-all-second-failed" ? 1U : 2U));
                if (!reject && scenario != "saved-all-second-failed") assert(saved_remaining == 0);
            } else {
                assert(controller.SelectSavedControlPeer(lease + 1));
                assert(!controller.SelectSavedControlPeer(lease));
                assert(!controller.ConfirmControlRemoval(lease, true));
                if (scenario == "saved-selected-stale") {
                    saved_allowed = false;
                    controller.Poll();
                    assert(controller.Snapshot().phase == Phase::kFailed && !forget_calls);
                    assert(!controller.ConfirmControlRemoval(lease + 1, true));
                } else if (scenario == "saved-reject") {
                    assert(controller.ConfirmControlRemoval(lease + 1, false));
                    assert(controller.Snapshot().phase == Phase::kCancelled);
                } else {
                    const bool gated = scenario == "saved-cancel-active" || scenario == "saved-pending-io";
                    if (gated) forget_gate.Block();
                    if (scenario == "saved-storage-failed") forget_error = WatchControlRemovalError::kStorage;
                    if (scenario == "saved-pending-io") forget_io_pending = true;
                    assert(controller.ConfirmControlRemoval(lease + 1, true));
                    if (gated) {
                        forget_gate.Await(); assert(!controller.StopWithin(0) && HasReservation());
                        forget_gate.Unblock();
                        if (forget_io_pending) {
                            Wait([] { return WatchAncsClient::Get().ControlRemovalSnapshot().phase == WatchControlRemovalPhase::kComplete; });
                            assert(!controller.StopWithin(0) && HasReservation());
                            forget_io_pending = false;
                        }
                    }
                    Reach(controller, scenario == "saved-storage-failed" ? Phase::kFailed : Phase::kControlRemoved);
                    assert(forget_calls == 1 && controller.Snapshot().control_peer_generation == lease + 1);
                }
            }
        }
        assert(controller.Snapshot().saved_peers.empty() && !controller.Active());
        assert(!identity_reads && !http_requests && !writes && !commits && !live_window);
    } else if (scenario.rfind("forget-", 0) == 0) {
        forget_allowed = true;
        identity_present = encrypted = tls_ok = false;
        unix_seconds = 0;
        assert(!controller.OpenControlRemoval(0));
        assert(controller.OpenControlRemoval(lease));
        assert(controller.Snapshot().phase == Phase::kAwaitControlRemoval && HasReservation());
        assert(!controller.Snapshot().control_only && controller.Snapshot().removing_control);
        assert(!controller.ConfirmControlRemoval(lease + 1, true));
        assert(!controller.OpenControlRemoval(lease));
        if (scenario == "forget-reject") {
            assert(controller.ConfirmControlRemoval(lease, false));
            assert(controller.Snapshot().phase == Phase::kCancelled && !forget_calls);
        } else if (scenario == "forget-cancel-before" || scenario == "forget-expired" || scenario == "forget-clock-rollback") {
            if (scenario == "forget-cancel-before") controller.Cancel();
            else now = scenario == "forget-expired" ? 121000 : 999;
            assert(!controller.ConfirmControlRemoval(lease, true));
            controller.Poll();
            assert(controller.Snapshot().phase == (scenario == "forget-cancel-before" ? Phase::kCancelled : Phase::kFailed));
            assert(!forget_calls);
        } else {
            if (scenario == "forget-admission-failed") forget_allowed = false;
            if (scenario == "forget-storage-failed") forget_error = WatchControlRemovalError::kStorage;
            if (scenario == "forget-cancel-failed" || scenario == "forget-pending-io") forget_error = WatchControlRemovalError::kTimeout;
            if (scenario == "forget-pending-io") forget_io_pending = true;
            const bool gated = scenario == "forget-cancel-active" || scenario == "forget-stop-active" || scenario == "forget-cancel-failed";
            if (gated) forget_gate.Block();
            assert(controller.ConfirmControlRemoval(lease, true));
            assert(!controller.ConfirmControlRemoval(lease, true));
            if (gated) {
                forget_gate.Await();
                assert(forget_calls == 1);
                if (scenario == "forget-stop-active") assert(!controller.StopWithin(0));
                else controller.Cancel();
                controller.Poll();
                assert(controller.Active() && HasReservation());
                forget_gate.Unblock();
            }
            if (scenario == "forget-pending-io") {
                Wait([] { return WatchAncsClient::Get().ControlRemovalSnapshot().phase == WatchControlRemovalPhase::kFailed; });
                assert(!controller.StopWithin(0) && controller.Active() && HasReservation());
                forget_io_pending = false;
            }
            const bool failed = scenario == "forget-admission-failed" || forget_error != WatchControlRemovalError::kNone;
            if (scenario == "forget-stop-completed") {
                Wait([] { return WatchAncsClient::Get().ControlRemovalSnapshot().phase == WatchControlRemovalPhase::kComplete; });
                assert(controller.StopWithin(1000));
            } else Reach(controller, failed ? Phase::kFailed : Phase::kControlRemoved);
            assert(controller.Snapshot().phase == (failed ? Phase::kFailed : Phase::kControlRemoved));
            if (failed) assert(controller.Snapshot().error == (scenario == "forget-storage-failed" ? Error::kStorage : Error::kControlRemoval));
            assert(controller.Snapshot().control_peer_generation == lease);
        }
        assert(!identity_reads && !http_requests && !writes && !commits && !storage_opens && !live_window);
    } else if (scenario.rfind("remove-", 0) == 0) {
        if (scenario != "remove-absent") assert(SaveBinding(0, device, credential) == BindingStatus::kBound);
        writes = commits = 0;
        auto hooks = Hooks(); hooks.unix_seconds = {}; hooks.prepare_tls_clock = {};
        Controller removal({}, std::move(hooks));
        identity_present = tls_ok = false; unix_seconds = 0;
        if (scenario == "remove-unprotected") encrypted = false;
        if (scenario == "remove-cancel-read") binding_read_gate.Block();
        assert(removal.OpenBindingRemoval());
        if (scenario == "remove-cancel-read") {
            binding_read_gate.Await(); removal.Cancel();
            assert(!removal.StopWithin(0) && reserved);
            binding_read_gate.Unblock(); Reach(removal, Phase::kCancelled);
        } else if (scenario == "remove-absent" || scenario == "remove-unprotected") {
            Reach(removal, Phase::kFailed);
            assert(removal.Snapshot().error == (scenario == "remove-absent" ? Error::kNoBinding : Error::kStorage));
        } else {
            Reach(removal, Phase::kAwaitRemoval);
            const auto original = removal.Snapshot();
            assert(original.removing_binding && !original.control_only && original.binding_generation == 1 &&
                original.device_id == device && !original.offer.request && reserved);
            assert(!removal.Open(true) && !removal.OpenBindingRemoval());
            assert(!removal.ConfirmPeer(1, true) && !removal.ApproveAccount(1, true));
            assert(!removal.ConfirmBindingRemoval(0, true) && !removal.ConfirmBindingRemoval(2, true));
            assert(writes == 0 && ActiveBindingGeneration() == 1);
            if (scenario == "remove-reject") {
                assert(removal.ConfirmBindingRemoval(1, false));
                assert(removal.Snapshot().phase == Phase::kCancelled && writes == 0 && ActiveBindingGeneration() == 1);
            } else if (scenario == "remove-timeout" || scenario == "remove-clock-rollback") {
                now = scenario == "remove-timeout" ? now.load() + 120000 : 1;
                assert(!removal.ConfirmBindingRemoval(1, true));
                Reach(removal, Phase::kFailed);
                assert(removal.Snapshot().error == Error::kTimeout && writes == 0);
            } else {
                if (scenario == "remove-generation-conflict") {
                    assert(SaveBinding(1, device, credential) == BindingStatus::kBound);
                    writes = commits = 0;
                }
                if (scenario == "remove-cancel-before-write") on_storage_open = [&] { removal.Cancel(); };
                fail_write = scenario == "remove-write-failed";
                fail_commit = scenario == "remove-commit-failed" || scenario == "remove-cancel-commit-failed";
                const bool blocked = scenario == "remove-cancel-write" || scenario == "remove-stop-write" ||
                    scenario == "remove-cancel-commit-failed";
                if (blocked) write_gate.Block();
                assert(removal.ConfirmBindingRemoval(1, true));
                assert(!removal.ConfirmBindingRemoval(1, true));
                if (blocked) {
                    write_gate.Await(); assert(ActiveBindingGeneration() == 0 && reserved);
                    if (scenario == "remove-stop-write") assert(!removal.StopWithin(0));
                    else removal.Cancel();
                    removal.Poll(); assert(removal.Active() && released == 0);
                    write_gate.Unblock();
                }
                if (scenario == "remove-cancel-before-write") {
                    Reach(removal, Phase::kCancelled); assert(writes == 0 && ActiveBindingGeneration() == 1);
                } else if (scenario == "remove-generation-conflict") {
                    Reach(removal, Phase::kFailed);
                    assert(removal.Snapshot().error == Error::kConflict && writes == 0 && ActiveBindingGeneration() == 2);
                } else if (fail_write || fail_commit) {
                    Reach(removal, Phase::kFailed);
                    assert(removal.Snapshot().error == Error::kStorage && ActiveBindingGeneration() == 0);
                    Binding unknown; assert(LoadBinding(unknown) == BindingStatus::kStorageError);
                } else {
                    if (scenario == "remove-stop-completed") {
                        Wait([] { return commits == 1; }); assert(removal.StopWithin(1000));
                    }
                    Reach(removal, Phase::kBindingRemoved);
                    assert(removal.Snapshot().binding_generation == 2 && !reserved);
                    Binding cleared; assert(LoadBinding(cleared) == BindingStatus::kRemoved && cleared.generation == 2);
                    assert(cleared.device_id.empty() && cleared.credential.empty() && ActiveBindingGeneration() == 0);
                    assert(disk[1] == 2 && std::all_of(disk.begin() + 12, disk.end(), [](uint8_t v) { return v == 0; }));
                    assert(writes == 1 && commits == 1);
                    removal.Cancel(); assert(removal.StopWithin(0));
                    assert(removal.Snapshot().phase == Phase::kBindingRemoved);
                }
            }
        }
        assert(!removal.Active() && !live_window && next_window == 0 && http_requests == 0 && identity_reads == 0 &&
            proofs == 0 && admissions == 0 && bound_reports == 0 && acquired == released);
        if (scenario == "remove-success") {
            identity_present = tls_ok = true; unix_seconds = 1789401600;
            writes = commits = 0;
            StartVerification(controller); Reach(controller, Phase::kBound);
            Binding rebound; assert(LoadBinding(rebound) == BindingStatus::kBound && rebound.generation == 3);
            assert(controller.Snapshot().binding_generation == 3 && rebound.credential == credential);
            now += 5000; controller.Poll();
        }
    } else if (scenario.rfind("control-", 0) == 0) {
        auto hooks = Hooks();
        hooks.unix_seconds = {}; hooks.prepare_tls_clock = {};
        identity_present = encrypted = tls_ok = false; unix_seconds = 0;
        Controller local({}, std::move(hooks));
        if (scenario == "control-open-failed") native_open_ok = false;
        const bool opened = local.Open(true);
        if (scenario == "control-open-failed") {
            assert(!opened && local.Snapshot().phase == Phase::kFailed && !local.Active());
        } else {
            assert(opened && local.Active() && local.Snapshot().control_only);
            assert(local.Snapshot().phase == Phase::kAwaitPeer && stage == Transport::Stage::kClosed);
            assert(!local.ApproveAccount(1, true) && !local.ConfirmPeer(2, true));
            assert(local.ConfirmPeer(1, scenario != "control-reject"));
            if (scenario == "control-success" || scenario == "control-cancel-completed") {
                local_control_grant = live_window; live_window = 0; local_control_mode = false;
                if (scenario == "control-cancel-completed") local.Cancel();
            } else if (scenario == "control-cancel") {
                local.Cancel();
            } else if (scenario == "control-timeout") {
                now += 120000;
            } else if (scenario == "control-stop") {
                assert(local.StopWithin(0));
            } else { assert(scenario == "control-reject"); }
            local.Poll();
            assert(!local.Active() && !reserved);
            const auto state = local.Snapshot();
            if (scenario == "control-success") {
                assert(state.phase == Phase::kControlReady && state.control_peer_generation == lease);
                assert(local_control_grant && !state.binding_generation && state.device_id.empty() && !state.offer.request);
            } else if (scenario == "control-timeout") {
                assert(state.phase == Phase::kFailed && state.error == Error::kTimeout);
            } else { assert(state.phase == Phase::kCancelled && !local_control_grant); }
        }
        assert(acquired == released && storage_opens == 0 && identity_reads == 0 && http_requests == 0);
        assert(writes == 0 && commits == 0 && proofs == 0 && bound_reports == 0);
    } else if (scenario == "success") {
        StartVerification(controller); Reach(controller, Phase::kBound); AssertSaved();
        assert(controller.Active() && reserved && bound_reports == 1 && controller.Snapshot().binding_generation == 1);
        now += 4999; controller.Poll(); assert(controller.Active());
        now += 1; controller.Poll(); assert(!controller.Active() && !reserved);
        assert(controller.Snapshot().phase == Phase::kBound);
        assert(controller.Open()); Reach(controller, Phase::kFailed);
        assert(controller.Snapshot().error == Error::kAlreadyBound && http_requests == 1);
    } else if (scenario == "approval") {
        Offered(controller); for (int i = 0; i < 3; ++i) controller.Poll();
        assert(proofs == 0 && http_requests == 0);
        const auto old = controller.Snapshot().offer.request;
        const auto old_attempt = controller.Snapshot().attempt;
        assert(old_attempt != 0);
        assert(controller.ApproveAccount(old, false)); assert(!controller.Active()); AssertNoBinding();
        Offered(controller); assert(controller.Snapshot().offer.request > old);
        assert(controller.Snapshot().attempt > old_attempt);
        assert(!controller.ApproveAccount(old, true)); assert(controller.StopWithin(1000)); AssertNoBinding();
    } else if (scenario == "invalid-challenge") {
        Ready(controller); Send(Transport::Kind::kChallenge, Payload(1789401600)); Reach(controller, Phase::kFailed);
        assert(controller.Snapshot().error == Error::kInvalidChallenge); AssertNoBinding();
    } else if (scenario == "clock" || scenario == "identity" || scenario == "unprotected" || scenario == "open-failed") {
        if (scenario == "clock") unix_seconds = 0;
        if (scenario == "identity") identity_present = false;
        if (scenario == "unprotected") encrypted = false;
        if (scenario == "open-failed") gatt_open_ok = false;
        assert(controller.Open()); Reach(controller, Phase::kFailed);
        assert(controller.Snapshot().error == (scenario == "clock" ? Error::kClock :
            scenario == "open-failed" ? Error::kUnavailable : Error::kIdentity));
        assert(!live_window); AssertNoBinding();
    } else if (scenario == "cancel-prepare") {
        identity_gate.Block(); assert(controller.Open()); identity_gate.Await();
        assert(!controller.StopWithin(0) && controller.Active() && reserved && released == 0);
        identity_gate.Unblock(); Reach(controller, Phase::kCancelled); AssertNoBinding();
    } else if (scenario == "cancel-acquire") {
        on_acquire = [&] { controller.Cancel(); };
        assert(controller.Open()); Reach(controller, Phase::kCancelled);
        assert(identity_reads == 0 && !live_window); AssertNoBinding();
    } else if (scenario == "acquire-failed") {
        acquire_ok = false; assert(!controller.Open());
        assert(controller.Snapshot().error == Error::kUnavailable && acquired == 0);
        assert(controller.StopWithin(0) && controller.Snapshot().phase == Phase::kFailed); AssertNoBinding();
    } else if (scenario == "destructor-join") {
        auto owned = std::make_unique<Controller>(endpoints, Hooks());
        http_gate.Block(); StartVerification(*owned); http_gate.Await();
        std::thread unblock([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(reserved && http_live && released == 0);
            http_gate.Unblock();
        });
        owned.reset(); unblock.join(); AssertNoBinding();
        assert(http_cleanups == 1 && released == 1);
    } else if (scenario == "cancel-http" || scenario == "disconnect-http" || scenario == "recording-http") {
        http_gate.Block(); StartVerification(controller); http_gate.Await();
        if (scenario == "cancel-http") controller.Cancel();
        if (scenario == "disconnect-http") Disconnect();
        if (scenario == "recording-http") eligible = false;
        controller.Poll(); assert(controller.Active() && reserved && released == 0 && http_live);
        http_gate.Unblock(); Reach(controller, Phase::kCancelled); AssertNoBinding();
        assert(http_cleanups == 1 && !http_live);
    } else if (scenario == "cancel-before-write") {
        on_storage_open = [&] { controller.Cancel(); };
        StartVerification(controller); Reach(controller, Phase::kCancelled); AssertNoBinding();
        assert(http_cleanups == 1 && admissions == 0);
    } else if (scenario == "cancel-during-write" || scenario == "disconnect-during-write" || scenario == "stop-during-write") {
        write_gate.Block(); StartVerification(controller); write_gate.Await();
        assert(admissions == 1 && commits == 0 && bound_reports == 0 && reserved);
        if (scenario == "disconnect-during-write") Disconnect();
        else if (scenario == "stop-during-write") assert(!controller.StopWithin(0));
        else controller.Cancel();
        controller.Poll(); assert(controller.Active() && released == 0);
        write_gate.Unblock(); Reach(controller, Phase::kBound); AssertSaved();
        assert(!controller.Active() && bound_reports == 0 && controller.Snapshot().binding_generation == 1);
    } else if (scenario == "wrong-account" || scenario == "unauthorized" || scenario == "tls-clock" || scenario == "wrong-wire") {
        if (scenario == "wrong-account") receipt.replace(receipt.find(user), user.size(), challenge);
        if (scenario == "unauthorized") http_status = 403;
        if (scenario == "tls-clock") tls_ok = false;
        if (scenario == "wrong-wire") { Signed(controller); Send(Transport::Kind::kCredential, credential, 20); }
        else StartVerification(controller);
        Reach(controller, Phase::kFailed); AssertNoBinding();
        assert(controller.Snapshot().error == (scenario == "tls-clock" ? Error::kClock :
            scenario == "wrong-wire" ? Error::kRejected : Error::kVerification));
    } else if (scenario == "write-failed" || scenario == "commit-failed" || scenario == "cancel-commit-failed" ||
               scenario == "stop-commit-failed") {
        fail_write = scenario == "write-failed"; fail_commit = !fail_write;
        if (scenario == "cancel-commit-failed" || scenario == "stop-commit-failed") write_gate.Block();
        StartVerification(controller);
        if (scenario == "cancel-commit-failed") { write_gate.Await(); controller.Cancel(); write_gate.Unblock(); }
        if (scenario == "stop-commit-failed") {
            write_gate.Await(); assert(!controller.StopWithin(0)); write_gate.Unblock();
            assert(controller.StopWithin(1000));
        }
        Reach(controller, Phase::kFailed);
        assert(controller.Snapshot().error == Error::kStorage && bound_reports == 0);
        Binding result; assert(LoadBinding(result) == BindingStatus::kStorageError);
        assert(controller.StopWithin(0) && controller.StopWithin(0));
        assert(controller.Snapshot().phase == Phase::kFailed && controller.Snapshot().error == Error::kStorage);
    } else if (scenario == "expired-window") {
        Ready(controller); now += 120000; controller.Poll(); assert(!controller.Active()); AssertNoBinding();
        assert(controller.Snapshot().phase == Phase::kFailed && controller.Snapshot().error == Error::kTimeout);
    } else if (scenario == "generation-conflict") {
        on_http_cleanup = [] { assert(RemoveBinding(0) == BindingStatus::kRemoved); };
        StartVerification(controller); Reach(controller, Phase::kFailed);
        assert(controller.Snapshot().error == Error::kConflict && admissions == 0 && bound_reports == 0);
        Binding result; assert(LoadBinding(result) == BindingStatus::kRemoved && result.generation == 1);
    } else if (scenario == "stop-completed-write") {
        StartVerification(controller); Wait([] { return commits == 1; });
        assert(controller.StopWithin(1000)); AssertSaved();
        assert(controller.Snapshot().phase == Phase::kBound && controller.Snapshot().binding_generation == 1);
        assert(controller.StopWithin(0) && released == 1);
    } else { assert(false && "Unknown scenario"); }
    assert(!controller.Active() && !reserved && acquired == released && !http_live);
    std::cout << "PASS: pairing controller " << scenario << '\n';
    return 0;
}
