#include "watch/watch_voice_pairing.h"
#include "nvs.h"
#include <sodium.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>
#include <vector>

namespace {
using namespace watch_voice;
constexpr char device[] = "12345678-1234-4234-8234-123456789abc";
constexpr char user[] = "22345678-1234-4234-8234-123456789abc";
constexpr char challenge[] = "32345678-1234-4234-8234-123456789abc";
std::vector<uint8_t> record;
bool encrypted = true, present = true, failed = false;
unsigned handles = 0, reads = 0;
std::string Payload(uint64_t expiry = 1789401720) {
    return std::string("{\"domain\":\"audream-pairing-v1\",\"device_id\":\"") + device +
        "\",\"user_id\":\"" + user + "\",\"challenge_id\":\"" + challenge +
        "\",\"nonce\":\"" + std::string(43, 'x') + "\",\"expires_at\":" + std::to_string(expiry) + "}";
}
void Initialize() {
    record.assign(233, 0);
    std::copy_n("AID1", 4, record.begin());
    std::copy_n(device, 36, record.begin() + 4);
    record[40] = 8;
    std::copy_n("WATCH-01", 8, record.begin() + 41);
    std::fill(record.begin() + 201, record.end(), 9);
    std::array<uint8_t, 64> secret{};
    assert(crypto_sign_seed_keypair(record.data() + 169, secret.data(), record.data() + 201) == 0);
    sodium_memzero(secret.data(), secret.size());
}
std::string Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
}

bool esp_flash_encryption_enabled() { return encrypted; }
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* value) {
    assert(std::string(name) == "audream_ident" && mode == NVS_READONLY && handles == 0);
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    if (failed) return ESP_FAIL;
    *value = 1; ++handles;
    return ESP_OK;
}
void nvs_close(nvs_handle_t value) { assert(value == 1 && handles == 1); --handles; }
esp_err_t nvs_get_blob(nvs_handle_t value, const char* key, void* data, size_t* size) {
    assert(value == 1 && handles == 1 && std::string(key) == "identity_v1");
    if (!data) { *size = record.size(); return ESP_OK; }
    if (*size < record.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    ++reads;
    std::memcpy(data, record.data(), record.size());
    *size = record.size();
    return ESP_OK;
}

int main(int argc, char** argv) {
    assert(sodium_init() >= 0);
    Initialize();
    DeviceIdentity identity;
#if !CONFIG_NVS_ENCRYPTION
    assert(LoadDeviceIdentity(identity) == IdentityStatus::kProtectionRequired && reads == 0);
    PairingSession unprotected_session;
    assert(!unprotected_session.OpenLocalWindow(1000));
    std::cout << "PASS: unprotected firmware cannot read or sign with a factory identity\n";
    return 0;
#endif
    if (argc == 5) {
        const auto bytes = Read(argv[1]);
        record.assign(bytes.begin(), bytes.end());
        const auto payload = Read(argv[2]);
        PairingSession session;
        assert(session.OpenLocalWindow(1000));
        const auto offer = session.Offer(7, payload, std::stoull(argv[4]), 1001);
        assert(offer.request && session.ApproveLocally(offer.request, 1002));
        std::array<uint8_t, 64> proof{};
        assert(session.TakeProof(offer.request, 7, 1003, proof));
        std::ofstream output(argv[3], std::ios::binary);
        output.write(reinterpret_cast<const char*>(proof.data()), proof.size());
        assert(output.good());
        return 0;
    }
    assert(LoadDeviceIdentity(identity) == IdentityStatus::kOk && identity.device_id == device && identity.hardware_id == "WATCH-01");
    encrypted = false;
    const auto before = reads;
    assert(LoadDeviceIdentity(identity) == IdentityStatus::kProtectionRequired && identity.device_id.empty() && reads == before);
    encrypted = true; present = false;
    assert(LoadDeviceIdentity(identity) == IdentityStatus::kAbsent);
    present = true; failed = true;
    assert(LoadDeviceIdentity(identity) == IdentityStatus::kStorageError);
    failed = false;
    for (size_t offset : {size_t(0), size_t(40), size_t(168), size_t(169), size_t(201)}) {
        const auto old = record[offset];
        record[offset] = offset == 40 ? 0 : old ^ 0xff;
        assert(LoadDeviceIdentity(identity) == IdentityStatus::kInvalid && identity.device_id.empty());
        record[offset] = old;
    }
    assert(handles == 0);
    std::cout << "PASS: protected read-only identity checks record shape, device identity and seed/public consistency\n";
    PairingSession session;
    const auto payload = Payload();
    std::array<uint8_t, 64> proof{};
    assert(!session.Offer(7, payload, 1789401600, 1000).request);
    assert(session.OpenLocalWindow(1000));
    assert(!session.Offer(0, payload, 1789401600, 1001).request);
    for (const auto& invalid : {Payload(1789401600), Payload(1789401721), std::string("{\"nested\":{") + payload + "}}",
            payload.substr(0, payload.size() - 1) + ",\"domain\":\"audream-pairing-v1\"}",
            payload + " trailing", std::string(1025, 'x')}) {
        assert(!session.Offer(7, invalid, 1789401600, 1001).request);
    }
    auto other = payload;
    other.replace(other.find(device), 36, user);
    assert(!session.Offer(7, other, 1789401600, 1001).request);
    other = payload;
    other.replace(other.find("audream-pairing-v1"), 18, "audream-command-v1");
    assert(!session.Offer(7, other, 1789401600, 1001).request);
    auto offer = session.Offer(7, payload, 1789401600, 1001);
    assert(offer.request && offer.user_id == user && offer.challenge_id == challenge);
    assert(!session.Offer(8, payload, 1789401600, 1002).request);
    assert(!session.TakeProof(offer.request, 7, 1002, proof));
    assert(!session.ApproveLocally(offer.request + 1, 1003));
    assert(session.ApproveLocally(offer.request, 1003));
    assert(!session.TakeProof(offer.request, 8, 1004, proof));
    assert(session.TakeProof(offer.request, 7, 1005, proof));
    assert(crypto_sign_verify_detached(proof.data(), reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), record.data() + 169) == 0);
    assert(!session.TakeProof(offer.request, 7, 1006, proof));
    assert(std::all_of(proof.begin(), proof.end(), [](uint8_t byte) { return byte == 0; }));
    std::cout << "PASS: exact challenge is signed once only after local approval for the same secure peer\n";
    for (int reason = 0; reason < 5; ++reason) {
        assert(session.OpenLocalWindow(2000));
        offer = session.Offer(7, payload, 1789401600, 2001);
        assert(offer.request && session.ApproveLocally(offer.request, 2002));
        if (reason == 0) session.Cancel();
        if (reason == 1) session.Disconnect(7);
        if (reason == 2) assert(session.OpenLocalWindow(3000));
        if (reason == 3) record[4] = '9';
        const auto now = reason == 4 ? 122000 : 3001;
        assert(!session.TakeProof(offer.request, 7, now, proof));
        record[4] = device[0];
    }
    assert(session.OpenLocalWindow(2000));
    offer = session.Offer(7, payload, 1789401600, 2001);
    assert(session.ApproveLocally(offer.request, 2002));
    assert(!session.TakeProof(offer.request, 7, 1999, proof));
    std::cout << "PASS: cancel, disconnect, replacement, changed identity, deadline and clock rollback invalidate approval\n";
    assert(session.OpenLocalWindow(4000));
    offer = session.Offer(7, payload, 1789401600, 4001);
    assert(session.ApproveLocally(offer.request, 4002));
    std::atomic<unsigned> successes{0};
    const auto consume = [&]() {
        std::array<uint8_t, 64> local{};
        if (session.TakeProof(offer.request, 7, 4003, local)) ++successes;
    };
    std::thread first(consume), second(consume);
    first.join(); second.join();
    assert(successes == 1);
    const auto end = std::numeric_limits<uint64_t>::max();
    assert(!session.OpenLocalWindow(end - 119999));
    assert(session.OpenLocalWindow(end - 120000));
    offer = session.Offer(7, payload, 1789401600, end - 1);
    assert(offer.request && session.ApproveLocally(offer.request, end - 1));
    assert(!session.TakeProof(offer.request, 7, end, proof));
    std::cout << "PASS: concurrent proof consumption is single-use and deadlines cannot overflow\n";
}
