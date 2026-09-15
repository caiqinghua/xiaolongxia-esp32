#include "watch_blufi_scope.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_uint_fast64_t grant;
static atomic_uint phase;
uint64_t audream_blufi_transport_lease(void) { return atomic_load(&grant); }

static void* OtherTask(void* argument) {
    (void)argument;
    while (atomic_load(&phase) != 1) sched_yield();
    // The original task deliberately holds an invalid scope throughout this task.
    assert(audream_blufi_operation_lease() == 200);
    audream_blufi_scope_t previous = audream_blufi_scope_enter(200);
    assert(!previous.active && audream_blufi_operation_lease() == 200);
    atomic_store(&phase, 2);
    while (atomic_load(&phase) != 3) sched_yield();
    assert(audream_blufi_operation_lease() == 200);
    audream_blufi_scope_leave(previous);
    return NULL;
}

int main(void) {
    assert(audream_blufi_operation_lease() == 0);
    atomic_store(&grant, 100);
    assert(audream_blufi_operation_lease() == 100);
    audream_blufi_scope_t outer = audream_blufi_scope_enter(100);
    assert(!outer.active && audream_blufi_operation_lease() == 100);
    audream_blufi_scope_t inner = audream_blufi_scope_enter(100);
    assert(inner.active && inner.lease == 100 && audream_blufi_operation_lease() == 100);
    audream_blufi_scope_leave(inner);
    atomic_store(&grant, 101);
    assert(audream_blufi_operation_lease() == 0);
    inner = audream_blufi_scope_enter(101);
    assert(audream_blufi_operation_lease() == 0);
    audream_blufi_scope_leave(inner);
    assert(audream_blufi_operation_lease() == 0);
    audream_blufi_scope_leave(outer);
    assert(audream_blufi_operation_lease() == 101);

    outer = audream_blufi_scope_enter(0);
    assert(audream_blufi_operation_lease() == 0);
    inner = audream_blufi_scope_enter(101);
    assert(audream_blufi_operation_lease() == 0);
    audream_blufi_scope_leave(inner);
    audream_blufi_scope_leave(outer);
    assert(audream_blufi_operation_lease() == 101);

    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        atomic_store(&grant, 200);
        atomic_store(&phase, 0);
        outer = audream_blufi_scope_enter(199);
        pthread_t other;
        assert(pthread_create(&other, NULL, OtherTask, NULL) == 0);
        atomic_store(&phase, 1);
        while (atomic_load(&phase) != 2) sched_yield();
        assert(audream_blufi_operation_lease() == 0);
        inner = audream_blufi_scope_enter(200);
        assert(audream_blufi_operation_lease() == 0);
        audream_blufi_scope_leave(inner);
        atomic_store(&phase, 3);
        assert(pthread_join(other, NULL) == 0);
        assert(audream_blufi_operation_lease() == 0);
        audream_blufi_scope_leave(outer);
        assert(audream_blufi_operation_lease() == 200);
    }
    puts("PASS: execution grants remain immutable across nesting/revocation and isolated across concurrent tasks (100 runs)");
}
