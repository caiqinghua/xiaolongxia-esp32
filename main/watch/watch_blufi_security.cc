#include "watch_blufi_security.h"
#include "watch_ancs_client.h"

extern "C" uint64_t audream_blufi_control_lease(uint16_t connection, const uint8_t* address) {
    return WatchAncsClient::Get().ControlPeerLease(connection, address);
}
