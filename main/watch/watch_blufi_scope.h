#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t lease;
    bool active;
} audream_blufi_scope_t;

// A zero-lease scope deliberately denies traffic; it is not an unscoped call.
audream_blufi_scope_t audream_blufi_scope_enter(uint64_t lease);
void audream_blufi_scope_leave(audream_blufi_scope_t previous);
uint64_t audream_blufi_operation_lease(void);

#ifdef __cplusplus
}
#endif
