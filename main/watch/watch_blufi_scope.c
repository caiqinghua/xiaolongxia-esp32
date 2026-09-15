#include "watch_blufi_scope.h"
#include "watch_blufi_security.h"

// BTA receive and BTC dispatch execute on different SDK tasks.
static __thread audream_blufi_scope_t current_scope;

audream_blufi_scope_t audream_blufi_scope_enter(uint64_t lease)
{
    const audream_blufi_scope_t previous = current_scope;
    // A nested operation cannot upgrade its parent's authorization.
    if (current_scope.active && current_scope.lease != lease) lease = 0;
    current_scope = (audream_blufi_scope_t){.lease = lease, .active = true};
    return previous;
}

void audream_blufi_scope_leave(audream_blufi_scope_t previous)
{
    current_scope = previous;
}

uint64_t audream_blufi_operation_lease(void)
{
    const uint64_t current = audream_blufi_transport_lease();
    if (!current_scope.active) return current;
    return current_scope.lease && current_scope.lease == current ? current_scope.lease : 0;
}
