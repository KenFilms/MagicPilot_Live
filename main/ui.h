/*
 * Camera UI: touch input, actions and redraws of the overlay (see ui_draw.h for the drawing).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Starts the UI task (touch polling, overlay redraws). Call after the display is up. */
void ui_start(void);

/* A full-screen page (menu, presets, slate, keyboard, PIN) covers the picture: the renderer can skip video frames */
bool ui_fullscreen(void);

/* HDMI picture present or gone, with a short description of the format (shown in the menu) */
void ui_set_video(bool on, const char *info);


/* 2x zoom (pinch): true while zoomed, with the view point (0..1280, 0..720 of the unzoomed picture)
 * to show in the middle. Called by the renderer every frame. */
bool ui_zoom(float *vx, float *vy);

/* Live view setting: picture from the HDMI capture adapter on USB-A, or remote only. The handler
 * (called from the UI task) applies a change. */
bool ui_live_view(void);
void ui_set_live_view_handler(void (*handler)(bool on));
