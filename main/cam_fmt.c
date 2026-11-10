/*
 * Formatting of camera values for the display. No hardware dependencies (also used by the
 * desktop preview).
 */

#include <stdio.h>
#include <math.h>
#include "cam.h"

float cam_iris_fnumber(int16_t av)
{
    return powf(1.41421356f, av / 2048.0f);
}

int16_t cam_iris_av(float fnumber)
{
    return (int16_t)lroundf(2.0f * log2f(fnumber) * 2048.0f);
}

void cam_fmt_fps(char *out, size_t size, int16_t fps, bool mrate)
{
    /* The camera reports 23.98 as 23 (and so on), or as 24 with the M-rate flag */
    const bool below = fps == 23 || fps == 29 || fps == 47 || fps == 59;
    if (mrate || below) {
        const int base = below ? fps + 1 : fps;
        /* base x 1000 / 1001 (the NTSC rate), in hundredths, rounded */
        const int hundredths = (base * 100000 + 500) / 1001;
        snprintf(out, size, "%d.%02d", hundredths / 100, hundredths % 100);
    } else {
        snprintf(out, size, "%d", fps);
    }
}

void cam_fmt_shutter(char *out, size_t size, int32_t angle100)
{
    const int32_t tenths = (angle100 + 5) / 10;
    if (tenths % 10 == 0) {
        snprintf(out, size, "%ld°", (long)(tenths / 10));
    } else {
        snprintf(out, size, "%ld.%ld°", (long)(tenths / 10), (long)(tenths % 10));
    }
}

void cam_fmt_shutter_speed(char *out, size_t size, int32_t speed)
{
    snprintf(out, size, "1/%ld", (long)speed);
}

void cam_fmt_iris(char *out, size_t size, int16_t av)
{
    const float f = cam_iris_fnumber(av);
    snprintf(out, size, f < 10 ? "f%.2f" : "f%.1f", f);
}

void cam_fmt_timecode(char *out, size_t size, const uint8_t tc[4])
{
    snprintf(out, size, "%02X:%02X:%02X:%02X", tc[3], tc[2], tc[1], tc[0]);
}
