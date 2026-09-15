#pragma once
#include "FreeRTOS.h"
#include <cassert>
#include <functional>

struct HostEventGroup { EventBits_t bits = 0; };
using EventGroupHandle_t = HostEventGroup*;
inline int host_event_groups = 0;
inline std::function<void()> host_wait_hook;
inline EventGroupHandle_t xEventGroupCreate() { ++host_event_groups; return new HostEventGroup; }
inline void vEventGroupDelete(EventGroupHandle_t group) { --host_event_groups; delete group; }
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { return group->bits |= bits; }
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    auto old = group->bits;
    group->bits &= ~bits;
    return old;
}
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, bool clear, bool, unsigned) {
    if (host_wait_hook) host_wait_hook();
    const auto result = group->bits;
    if (clear) group->bits &= ~bits;
    return result;
}
