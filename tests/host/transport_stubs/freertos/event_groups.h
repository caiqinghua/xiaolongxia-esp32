#pragma once
#include "FreeRTOS.h"
#include <condition_variable>
#include <mutex>
struct HostTransportEvents {
    std::mutex mutex;
    std::condition_variable cv;
    EventBits_t bits = 0;
};
using EventGroupHandle_t = HostTransportEvents*;
EventGroupHandle_t xEventGroupCreate();
void vEventGroupDelete(EventGroupHandle_t);
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupGetBits(EventGroupHandle_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, bool, bool, TickType_t);
