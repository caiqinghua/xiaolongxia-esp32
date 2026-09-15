#pragma once
#include "FreeRTOS.h"
#include <thread>
struct HostTransportTask { std::thread thread; };
using TaskHandle_t = HostTransportTask*;
BaseType_t xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*);
TaskHandle_t xTaskGetCurrentTaskHandle();
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
