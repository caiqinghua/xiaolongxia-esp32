#pragma once

#include "bta_gatts_int.h"
#include "watch_blufi_notify.h"
#include <stddef.h>
#include <string.h>

// Standard BTA indication producers zero these unused header fields.
enum { AUDREAM_BLUFI_BTA_TAG = 0xa0b1 };
typedef struct {
    tBTA_GATTS_API_INDICATION packet;
    uint64_t lease;
    uint64_t subscription;
} audream_blufi_bta_message_t;

_Static_assert(sizeof(audream_blufi_bta_message_t) <= UINT16_MAX, "BTA envelope exceeds header length");

static inline bool audream_blufi_bta_tagged(const tBTA_GATTS_API_INDICATION* packet)
{
    return packet->hdr.offset == AUDREAM_BLUFI_BTA_TAG;
}

static inline bool audream_blufi_bta_admit(const tBTA_GATTS_API_INDICATION* packet)
{
    if (!audream_blufi_bta_tagged(packet)) return !audream_blufi_is_notify_handle(packet->attr_id);
    if (packet->hdr.len != sizeof(audream_blufi_bta_message_t) || packet->need_confirm ||
        !packet->len || packet->len > BTA_GATT_MAX_ATTR_LEN) return false;
    uint64_t lease = 0;
    memcpy(&lease, (const uint8_t*)packet + offsetof(audream_blufi_bta_message_t, lease), sizeof(lease));
    uint64_t subscription = 0;
    memcpy(&subscription, (const uint8_t*)packet + offsetof(audream_blufi_bta_message_t, subscription), sizeof(subscription));
    return subscription && subscription == audream_blufi_notify_token(packet->hdr.layer_specific, packet->attr_id, lease);
}
