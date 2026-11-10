#pragma once
#include "FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
#define xSemaphoreTake(s, t) ((void)(s), (void)(t), 1)
#define xSemaphoreGive(s) ((void)(s), 1)
