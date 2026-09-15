#pragma once
#include <freertos/task.h>
BaseType_t xTaskCreateWithCaps(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*, unsigned);
void vTaskDeleteWithCaps(TaskHandle_t);
