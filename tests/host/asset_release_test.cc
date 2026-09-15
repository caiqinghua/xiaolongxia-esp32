#include "asset_release.h"
#include "asset_release_fixture.h"
#include "audio/assistant_model_bundle.h"
#include "firmware_release_fixture.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace asset_fixture;

int main(int argc, char** argv) {
    assert(sodium_init() >= 0);
    const auto bundle = Bundle();
    const auto payload = Payload(bundle);
    const auto envelope = Sign(payload);
    const auto release = Verify(envelope);
    assert(release && Accept(bundle));
    assert(release->MinimumFirmware() == 6 && release->MaximumFirmware() == 9);
    assert(release->WakeModel() == kWake && release->Version() == "1.0.0");
    assert(!Verify(Seal(payload)));
    assert(!firmware_release::Verify(envelope, "test-watch", 7, PublicKey()));
    assert(!asset_release::Verify(envelope, kCompatibility, 8, PublicKey()));
    assert(!asset_release::Verify(envelope, kCompatibility, 7, std::string(64, '0')));
    for (auto changed : {asset_release::Compatibility{"other-watch", kCompatibility.sr_abi, kWake, 7},
                         asset_release::Compatibility{"test-watch", "other-abi", kWake, 7},
                         asset_release::Compatibility{"test-watch", kCompatibility.sr_abi, "other-wake", 7}})
        assert(!asset_release::Verify(envelope, changed, 7, PublicKey()));
    for (auto seq : {0U, 5U, 10U, UINT32_MAX}) {
        auto compatibility = kCompatibility; compatibility.firmware_sequence = seq;
        assert(!asset_release::Verify(envelope, compatibility, 7, PublicKey()));
    }
    for (const auto* field : {"sequence", "size", "firmware_min", "firmware_max"})
        for (const auto* value : {"", "0", "-1", "01", "1e2", "1.0", " 8", "2147483648"})
            assert(!Verify(Sign(Change(payload, field, value))));
    for (const auto& field : std::vector<std::pair<const char*, const char*>>{
            {"schema", "audream-firmware-v1"}, {"format", "spiffs"}, {"sequence", "7"},
            {"size", "16777217"}, {"size", "57"}, {"firmware_min", "8"}, {"firmware_max", "6"},
            {"sha256", "bad"}, {"version", "bad version"}, {"url", "http://updates.example/a"},
            {"url", "https://user@updates.example/a"}, {"url", "https://updates.example/a?key=secret"}})
        assert(!Verify(Sign(Change(payload, field.first, field.second))));
    auto duplicate = payload; duplicate.insert(1, "\"mo\\u0064el\":\"test-watch\",");
    assert(!Verify(Sign(duplicate)));
    auto nul = payload; nul.replace(nul.find("test-watch"), 10, "test-watch\\u0000tail");
    assert(!Verify(Sign(nul)));
    assert(!Verify(envelope + "{}"));
    assert(!Verify(std::string(4097, ' ')));
    for (size_t i = 0; i < envelope.size(); ++i) {
        auto changed = envelope; changed[i] ^= 1;
        assert(!Verify(changed));
    }
    for (size_t size = 0; size < bundle.size(); ++size) {
        assert(!asset_release::ValidateBundle(*release, bundle.data(), size));
        assert(!asset_release::ValidateBundleStructure(bundle.data(), size, kWake));
    }
    for (size_t i = 0; i < bundle.size(); ++i) {
        auto changed = bundle; changed[i] ^= 1;
        assert(!asset_release::ValidateBundle(*release, changed.data(), changed.size()));
    }
    auto changed = bundle; changed.push_back(0); assert(!Accept(changed));
    changed = bundle; Name(changed, 56, "index.json"); Checksum(changed); assert(!Accept(changed));
    changed = bundle; U32(changed, 48, UINT32_MAX); Checksum(changed); assert(!Accept(changed));
    changed = bundle; U32(changed, 44, UINT32_MAX); Checksum(changed); assert(!Accept(changed));
    changed = bundle; changed.back() ^= 1; assert(!Accept(changed));
    const size_t model_start = 12 + 88 + audream::ReadBundleU32(bundle.data() + 92) + 2;
    changed = bundle; U32(changed, model_start + 72, 0); Checksum(changed); assert(!Accept(changed));
    assert(!Accept(Bundle(R"({"version":2,"srmodels":"srmodels.bin"})")));
    assert(!Accept(Bundle(R"({"version":1,"srmodels":"missing.bin"})")));
    assert(!Accept(Bundle(R"({"version":1,"srmodels":"srmodels.bin","srmodels":"srmodels.bin"})")));
    assert(!Accept(Bundle(R"({"version":1,"srmodels":"srmodels.bin"} {})")));
    assert(!Accept(Bundle("{}")));
    assert(!Accept(Bundle(std::string(65537, ' '))));
    assert(!Accept(Bundle("{\"version\":1,\"srmodels\":\"srmodels.bin\",\"nested\":" +
                         std::string(17, '[') + "0" + std::string(17, ']') + "}")));
    assert(!Accept(Bundle(R"({"version":1,"srmodels":"srmodels.bin"})", "wn9_other")));
    assert(!asset_release::ValidateBundleStructure(nullptr, bundle.size(), kWake));
    if (argc == 3) {
        std::ifstream manifest(argv[1]), binary(argv[2], std::ios::binary);
        assert(manifest && binary);
        const std::string text((std::istreambuf_iterator<char>(manifest)), {});
        const std::string bytes((std::istreambuf_iterator<char>(binary)), {});
        auto imported = Verify(text);
        assert(imported && asset_release::ValidateBundle(*imported, bytes.data(), bytes.size()));
        std::cout << "PASS: Python asset signer and native bundle verifier interoperability\n";
    }
    std::cout << "PASS: asset signatures, firmware compatibility, wake identity and bounded bundle validation\n";
}
