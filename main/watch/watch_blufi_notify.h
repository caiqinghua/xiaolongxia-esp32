#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audream_blufi_is_notify_handle(uint16_t attribute);
bool audream_blufi_notify_current(uint16_t connection, uint16_t attribute, uint64_t lease);
uint64_t audream_blufi_notify_token(uint16_t connection, uint16_t attribute, uint64_t lease);
bool audream_blufi_queue_notification(uint64_t lease, uint16_t connection, uint16_t attribute,
    int length, const uint8_t* data);

#ifdef __cplusplus
}
#endif
