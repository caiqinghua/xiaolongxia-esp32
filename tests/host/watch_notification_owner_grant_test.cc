#include "watch/watch_notification_owner_grant.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1], device = Read(root + "/device.txt");
    std::array<uint8_t, 32> challenge; challenge.fill(0x12);
    for (const auto bits : {2048, 3072, 4096}) {
        const auto pin = Read(root + "/pin-" + std::to_string(bits) + ".txt");
        const auto data = Read(root + "/proof-" + std::to_string(bits) + ".bin");
        const std::vector<uint8_t> proof(data.begin(), data.end());
        auto verify = [&](const std::vector<uint8_t>& bytes) {
            return watch_notification::VerifyOwnerGrant(pin, device, 7, challenge, bytes.data(), bytes.size());
        };
        assert(verify(proof) == 300);
        for (size_t length = 0; length < proof.size(); ++length) {
            assert(!watch_notification::VerifyOwnerGrant(pin, device, 7, challenge, proof.data(), length));
        }
        for (size_t position = 0; position < proof.size(); ++position) {
            auto changed = proof; changed[position] ^= 1; assert(!verify(changed));
        }
        auto extra = proof; extra.push_back(0); assert(!verify(extra));
        assert(!watch_notification::VerifyOwnerGrant(pin, device, 8, challenge, proof.data(), proof.size()));
        auto nonce = challenge; nonce[0] ^= 1;
        assert(!watch_notification::VerifyOwnerGrant(pin, device, 7, nonce, proof.data(), proof.size()));
        for (const auto& id : {std::string{}, device + "x", std::string("00000000-0000-0000-0000-000000000000")}) {
            assert(!watch_notification::VerifyOwnerGrant(pin, id, 7, challenge, proof.data(), proof.size()));
        }
        for (const auto& invalid : {std::string{}, pin + "\n", std::string(804, 'A'), std::string("AAAA"),
                                   Read(root + "/pin-" + std::to_string(bits == 2048 ? 3072 : 2048) + ".txt")}) {
            assert(!watch_notification::VerifyOwnerGrant(invalid, device, 7, challenge, proof.data(), proof.size()));
        }
        assert(!watch_notification::VerifyOwnerGrant(pin, device, 7, challenge, nullptr, proof.size()));
    }
    const auto pin = Read(root + "/pin-2048.txt");
    for (const auto name : {"lease-0.bin", "lease-1.bin", "lease-301.bin", "pss.bin"}) {
        const auto data = Read(root + "/" + name);
        const auto result = watch_notification::VerifyOwnerGrant(pin, device, 7, challenge,
            reinterpret_cast<const uint8_t*>(data.data()), data.size());
        if (std::string(name) == "lease-1.bin") assert(result == 1);
        else assert(!result);
    }
    std::puts("API-to-Mbed-TLS owner proof contract passed for RSA 2048/3072/4096");
}
