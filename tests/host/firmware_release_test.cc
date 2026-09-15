#include "firmware_release_fixture.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    using namespace release_fixture;
    assert(sodium_init() >= 0);
    const auto key = PublicKey();
    assert(key == "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8");
    const auto payload = Payload(std::string(5000, 'x'));
    const auto envelope = Seal(payload);
    const auto verify = [&](std::string_view text) { return firmware_release::Verify(text, "test-watch", 7, key); };
    const auto valid = verify(envelope);
    assert(valid && valid->Size() == 5000 && valid->Sequence() == 8 && valid->Version() == "3.0.0");
    assert(!firmware_release::Verify(envelope, "other-watch", 7, key));
    assert(!firmware_release::Verify(envelope, "test-watch", 8, key));
    assert(!firmware_release::Verify(envelope, "test-watch", 9, key));
    assert(!firmware_release::Verify(envelope, "test-watch", 7, ""));
    assert(!firmware_release::Verify(envelope, "test-watch", 7, std::string(64, '0')));
    auto other_key = key; other_key[0] = other_key[0] == '0' ? '1' : '0';
    assert(!firmware_release::Verify(envelope, "test-watch", 7, other_key));
    for (size_t i = 0; i < envelope.size(); ++i) {
        auto changed = envelope; changed[i] ^= 1;
        assert(!verify(changed));
    }
    for (const char* name : {"sequence", "size"}) {
        for (const char* value : {"", "0", "-1", "01", "1.0", "1e2", " 8", "+8", "4294967296", "999999999999999999"})
            assert(!verify(Seal(Change(payload, name, value))));
    }
    assert(!verify(Seal(Change(payload, "sequence", "7"))));
    assert(!verify(Seal(Change(payload, "sequence", "2147483648"))));
    assert(!verify(Seal(Change(payload, "size", "33554433"))));
    assert(!verify(Seal(Change(payload, "size", "395"))));
    assert(!verify(Seal(Change(payload, "schema", "other"))));
    assert(!verify(Seal(Change(payload, "model", "other"))));
    assert(!verify(Seal(Change(payload, "version", "bad version"))));
    assert(!verify(Seal(Change(payload, "version", std::string(32, 'v')))));
    assert(!verify(Seal(Change(payload, "sha256", std::string(64, 'g')))));
    for (const char* url : {"http://a/file", "https://user@a/file", "https://a/file?token=secret", "https://a/#x",
                            "https://a:0/file", "https://a/\\x", "https://a/\nx", "https://"})
        assert(!verify(Seal(Change(payload, "url", url))));
    for (const char* url : {"https://a/file", "https://a:443/file", "https://[::1]/firmware.bin"})
        assert(verify(Seal(Change(payload, "url", url))));
    auto duplicate = payload; duplicate.insert(1, "\"model\":\"test-watch\",");
    assert(!verify(Seal(duplicate)));
    auto escaped_duplicate = payload; escaped_duplicate.insert(1, "\"mo\\u0064el\":\"test-watch\",");
    assert(!verify(Seal(escaped_duplicate)));
    assert(!verify(Seal("{\"model\":{\"nested\":{}}}")));
    assert(!verify(Seal("[]")));
    assert(!verify(envelope + "{}"));
    assert(!verify(std::string(4097, ' ')));
    auto nul_model = payload;
    nul_model.replace(nul_model.find("test-watch"), 10, "test-watch\\u0000extra");
    assert(!verify(Seal(nul_model)));
    assert(firmware_release::DiscoveryUrl("https://updates.example", "test-watch", 7) ==
           "https://updates.example/v1/firmware/releases/test-watch/after/7");
    assert(firmware_release::DiscoveryUrl("https://updates.example/", "test-watch", 0) ==
           "https://updates.example/v1/firmware/releases/test-watch/after/0");
    for (const char* origin : {"http://updates.example", "https://updates.example/path", "https://a?x=1", "https://u@a"})
        assert(firmware_release::DiscoveryUrl(origin, "test-watch", 7).empty());
    for (const char* model : {"..", ".", "bad/model", "-model", "bad%2fmodel"})
        assert(firmware_release::DiscoveryUrl("https://updates.example", model, 7).empty());
    assert(firmware_release::DiscoveryUrl("https://updates.example", "test-watch", 2147483648U).empty());

    if (argc == 2) {
        std::ifstream stream(argv[1]);
        assert(stream);
        const std::string text((std::istreambuf_iterator<char>(stream)), {});
        assert(verify(text));
        std::cout << "PASS: Python cryptography signer accepted by native libsodium verifier\n";
    }
    std::cout << "PASS: authenticated metadata, model, sequence, bounded parsing and mutation rejection\n";
}
