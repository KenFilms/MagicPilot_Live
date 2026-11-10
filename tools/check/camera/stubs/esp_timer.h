/* The test's own clock (driver.c), so that answer deadlines can be passed on purpose */
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
