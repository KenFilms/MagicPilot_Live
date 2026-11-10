/*
 * Bluetooth LE link to the Blackmagic camera.
 *
 * The ESP32-P4 has no radio: the Bluetooth controller runs on the Tab5's ESP32-C6, connected over
 * SDIO (ESP-Hosted), and the P4 runs the Bluedroid host. The link scans for the camera, connects,
 * pairs with the camera's 6-digit PIN (asked for on the touch screen, once; the bond is kept in
 * NVS), subscribes to the camera's notifications and reconnects whenever the link drops.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Powers the ESP32-C6 and starts the Bluetooth task, which connects to the camera and reconnects when the link drops */
void cam_ble_start(void);

/*
 * Queue a control packet for the camera. A non-zero key lets a newer packet replace a queued one
 * with the same key (slider drags send only the latest value). With CAM_BLE_KEY_SUM in the key, the
 * 16-bit values add up instead (offsets: none is lost).
 */
#define CAM_BLE_KEY_SUM     0x4000
bool cam_ble_send(const uint8_t *packet, size_t len, uint16_t key);

/* Answer the camera's PIN request (cancel = refuse pairing) */
void cam_ble_submit_pin(uint32_t pin);
void cam_ble_cancel_pin(void);

/* Remove the stored pairing and look for the camera again */
void cam_ble_forget(void);

/* ESP-Hosted firmware version of the ESP32-C6, "" until known */
const char *cam_ble_cp_version(void);
