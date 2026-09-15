#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Connection is the native API connection ID, not the encoded BTA server ID.
// Memory-only lookup; this never initiates pairing or grants authorization.
uint64_t audream_blufi_control_lease(uint16_t connection, const uint8_t* address);

// Implemented by the pinned SDK host overlay; rejects a stale native peer.
uint64_t audream_blufi_transport_lease(void);

#ifdef __cplusplus
}
#endif
