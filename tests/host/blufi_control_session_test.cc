#include "boards/common/blufi_control_session.h"
#include <cassert>
#include <atomic>
#include <cstdio>
#include <thread>

int main() {
    const uint8_t address[]{1, 2, 3, 4, 5, 6};
    std::atomic<uint64_t> grant{0};
    unsigned lookups = 0;
    auto verify = [&](uint16_t connection, const uint8_t* peer) {
        ++lookups;
        assert(connection == 7 && std::equal(peer, peer + 6, address));
        return grant.load();
    };
    BlufiControlSession secure(true);
    assert(!secure.Capture(verify) && lookups == 0);
    secure.Connected(7, address);
    assert(!secure.Capture(verify));
    grant = 101;
    const auto queued = secure.Capture(verify);
    assert(queued && queued.authorization == 101 && secure.IsCurrent(queued, verify));
    auto altered = queued;
    altered.connection++;
    assert(!secure.IsCurrent(altered, verify));
    altered = queued; altered.address[0]++;
    assert(!secure.IsCurrent(altered, verify));
    altered = queued; altered.authorization = 0;
    assert(!secure.IsCurrent(altered, verify));
    grant = 0;
    assert(!secure.IsCurrent(queued, verify) && !secure.Capture(verify));
    grant = 102;
    assert(!secure.IsCurrent(queued, verify));
    const auto replacement = secure.Capture(verify);
    assert(replacement.authorization == 102 && secure.IsCurrent(replacement, verify));
    secure.Reset();
    assert(!secure.IsCurrent(replacement, verify) && !secure.Capture(verify));
    secure.Connected(7, address);
    assert(!secure.IsCurrent(replacement, verify));
    const auto reconnected = secure.Capture(verify);
    assert(reconnected.generation > replacement.generation);
    secure.Connected(7, nullptr);
    assert(!secure.Capture(verify) && !secure.IsCurrent(reconnected, verify));

    // Legacy firmware retains its protocol without invoking a security owner.
    BlufiControlSession legacy(false);
    const auto before = lookups;
    legacy.Connected(7, address);
    const auto legacy_request = legacy.Capture(verify);
    assert(legacy_request && legacy_request.authorization == 0);
    assert(legacy.IsCurrent(legacy_request, verify) && lookups == before);
    legacy.Reset(); legacy.Connected(7, address);
    assert(!legacy.IsCurrent(legacy_request, verify) && lookups == before);

    // Connection IDs, addresses and grants can all be reused after disconnect.
    // The application generation must still reject an already queued request.
    secure.Connected(7, address);
    const auto stale = secure.Capture(verify);
    std::thread reconnect([&] { secure.Reset(); secure.Connected(7, address); });
    reconnect.join();
    assert(!secure.IsCurrent(stale, verify));
    std::puts("PASS: BLUFI admission, grant revocation, queued response isolation and legacy connection lifetime");
}
