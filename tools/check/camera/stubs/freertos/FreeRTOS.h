/* Just enough of FreeRTOS for main/cam.c on a computer: one thread, so the critical sections do nothing */
#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED    0
#define taskENTER_CRITICAL(mux)         ((void)(mux))
#define taskEXIT_CRITICAL(mux)          ((void)(mux))
