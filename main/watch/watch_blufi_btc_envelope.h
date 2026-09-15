#pragma once

#include "btc_blufi_prf.h"
#include "watch_blufi_security.h"
#include "watch_blufi_scope.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Preserve the SDK argument layout at offset zero for its deep-copy/free hooks.
// Only BLUFI messages use this envelope; other BTC profiles retain their ABI.
typedef struct {
    union {
        btc_blufi_args_t call;
        esp_blufi_cb_param_t callback;
    } payload;
    uint64_t control_lease;
} audream_blufi_btc_envelope_t;

static inline bool audream_blufi_lifecycle_message(const btc_msg_t* msg)
{
    if (msg->sig == BTC_SIG_API_CALL)
        return msg->act == BTC_BLUFI_ACT_INIT || msg->act == BTC_BLUFI_ACT_DEINIT;
    if (msg->sig != BTC_SIG_API_CB) return false;
    return msg->act == ESP_BLUFI_EVENT_INIT_FINISH || msg->act == ESP_BLUFI_EVENT_DEINIT_FINISH ||
           msg->act == ESP_BLUFI_EVENT_BLE_CONNECT || msg->act == ESP_BLUFI_EVENT_BLE_DISCONNECT;
}

static inline uint64_t audream_blufi_message_lease(const btc_msg_t* msg)
{
    if (!msg || msg->pid != BTC_PID_BLUFI || audream_blufi_lifecycle_message(msg)) return 0;
    // BTC arguments start four bytes after the allocation, not on a uint64_t boundary.
    uint64_t lease = 0;
    memcpy(&lease, msg->arg + offsetof(audream_blufi_btc_envelope_t, control_lease), sizeof(lease));
    return lease;
}

static inline bool audream_blufi_message_current(const btc_msg_t* msg)
{
    if (!msg || msg->pid != BTC_PID_BLUFI) return false;
    if (audream_blufi_lifecycle_message(msg)) return true;
    const uint64_t lease = audream_blufi_message_lease(msg);
    return lease && lease == audream_blufi_operation_lease();
}

static inline void audream_blufi_dispatch_message(btc_msg_t* msg, void (*handler)(btc_msg_t*),
    btc_arg_deep_free_t cleanup)
{
    if (!audream_blufi_message_current(msg)) {
        if (msg && cleanup) cleanup(msg);
        return;
    }
    const audream_blufi_scope_t previous = audream_blufi_scope_enter(audream_blufi_message_lease(msg));
    handler(msg);
    audream_blufi_scope_leave(previous);
}

static inline bt_status_t audream_blufi_transfer_for_lease(btc_msg_t* msg, void* arg, int arg_len,
    btc_arg_deep_copy_t copy, btc_arg_deep_free_t cleanup, uint64_t expected_lease)
{
    if (!msg) return BT_STATUS_FAIL;
    if (msg->pid != BTC_PID_BLUFI) return btc_transfer_context(msg, arg, arg_len, copy, cleanup);
    audream_blufi_btc_envelope_t envelope = {0};
    if (arg_len < 0 || (size_t)arg_len > sizeof(envelope.payload) || (arg_len && !arg)) return BT_STATUS_FAIL;
    if (!audream_blufi_lifecycle_message(msg)) {
        if (!expected_lease || expected_lease != audream_blufi_operation_lease()) return BT_STATUS_FAIL;
        envelope.control_lease = expected_lease;
    }
    if (arg_len) memcpy(&envelope.payload, arg, (size_t)arg_len);
    return btc_transfer_context(msg, &envelope, sizeof(envelope), copy, cleanup);
}

static inline bt_status_t audream_blufi_transfer_context(btc_msg_t* msg, void* arg, int arg_len,
    btc_arg_deep_copy_t copy, btc_arg_deep_free_t cleanup)
{
    uint64_t lease = 0;
    if (msg && msg->pid == BTC_PID_BLUFI && !audream_blufi_lifecycle_message(msg))
        lease = audream_blufi_operation_lease();
    return audream_blufi_transfer_for_lease(msg, arg, arg_len, copy, cleanup, lease);
}

#define btc_transfer_context audream_blufi_transfer_context
