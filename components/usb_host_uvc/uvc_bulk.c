/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h> // For memcpy

#include "esp_log.h"

#include "uvc_stream.h" // For uvc_host_stream_pause()
#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"

static const char *TAG = "uvc-bulk";

static void bulk_complete_frame(uvc_stream_t *uvc_stream)
{
    // Clear the current frame before invoking the callback so no later packet can append to it.
    UVC_ENTER_CRITICAL();
    uvc_host_frame_t *this_frame = uvc_stream->dynamic.current_frame;
    uvc_stream->dynamic.current_frame = NULL;

    const bool invoke_fb_callback = (uvc_stream->dynamic.streaming && uvc_stream->constant.frame_cb && this_frame &&
                                     !uvc_stream->single_thread.skip_current_frame);
    UVC_EXIT_CRITICAL();

    bool return_frame = true;
    if (invoke_fb_callback) {
        return_frame = uvc_stream->constant.frame_cb(this_frame, uvc_stream->constant.cb_arg);
    }
    if (this_frame && return_frame) {
        uvc_host_frame_return(uvc_stream, this_frame);
    }
}

static void bulk_add_frame_data(uvc_stream_t *uvc_stream, const uint8_t *data, size_t data_len)
{
    if (uvc_stream->single_thread.skip_current_frame || data_len == 0) {
        return;
    }

    uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);
    if (!current_frame) {
        ESP_LOGE(TAG, "missing current frame while appending bulk payload");
        uvc_stream->single_thread.skip_current_frame = true;
        return;
    }

    esp_err_t ret = uvc_frame_add_data(current_frame, data, data_len);
    if (ret != ESP_OK) {
        // Frame buffer overflow
        uvc_stream->single_thread.skip_current_frame = true;

        // Inform the user about the overflow
        uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
        if (stream_cb) {
            const uvc_host_stream_event_data_t event = {
                .type = UVC_HOST_FRAME_BUFFER_OVERFLOW,
            };
            stream_cb(&event, uvc_stream->constant.cb_arg);
        }
    }
}

/*
 * Start assembling a new frame at the first data byte of a payload.
 * For MJPEG the frame must begin with the SOI marker, otherwise it is skipped.
 */
static void bulk_start_frame(uvc_stream_t *uvc_stream, const uint8_t *data, size_t data_len)
{
    uvc_stream->single_thread.bulk_in_frame = true;
    uvc_stream->single_thread.skip_current_frame = false;

    if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG &&
            (data_len < 2 || data[0] != JPEG_MARKER || data[1] != JPEG_SOI)) {
        ESP_LOGD(TAG, "invalid MJPEG SOI");
        uvc_stream->single_thread.skip_current_frame = true;
        return;
    }

    UVC_ENTER_CRITICAL();
    const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame);
    if (need_new_frame) {
        UVC_EXIT_CRITICAL();
        uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
        if (uvc_stream->dynamic.current_frame == NULL) {
            uvc_stream->single_thread.skip_current_frame = true;
            uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
            if (stream_cb) {
                const uvc_host_stream_event_data_t event = {
                    .type = UVC_HOST_FRAME_BUFFER_UNDERFLOW,
                };
                stream_cb(&event, uvc_stream->constant.cb_arg);
            }
        }
    } else {
        if (uvc_stream->dynamic.current_frame) {
            uvc_frame_reset(uvc_stream->dynamic.current_frame);
        }
        UVC_EXIT_CRITICAL();
    }
}

static void bulk_end_frame(uvc_stream_t *uvc_stream)
{
    if (!uvc_stream->single_thread.bulk_in_frame) {
        return;
    }
    uvc_stream->single_thread.bulk_in_frame = false;
    bulk_complete_frame(uvc_stream);
}

/**
 * @brief Callback function for handling Bulk USB transfers from a UVC camera.
 *
 * Per UVC 1.5 (section 2.4.3.2.2), a bulk video stream is a sequence of payload transfers. Each payload
 * starts with a payload header and ends either with a short packet or when dwMaxPayloadTransferSize bytes
 * have been received. A frame consists of one or more payloads; a new frame is signalled by a toggle of the
 * FID bit and/or the EOF bit of the last payload.
 *
 * A USB transfer (URB) completes when its buffer is full or a short packet arrives, so a URB can contain the
 * tail of one payload followed by the start of the next one. The payload boundaries are therefore tracked by
 * counting bytes against dwMaxPayloadTransferSize, and every payload header is stripped from the frame data.
 *
 * @param[in] transfer Pointer to the completed USB transfer structure.
 */
void bulk_transfer_callback(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;

    switch (transfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
    case USB_TRANSFER_STATUS_ERROR:
    case USB_TRANSFER_STATUS_OVERFLOW:
    case USB_TRANSFER_STATUS_STALL:
        ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
        break;
    case USB_TRANSFER_STATUS_TIMED_OUT:
    case USB_TRANSFER_STATUS_SKIPPED: // Should never happen to BULK transfer
    default:
        assert(false);
    }

    if (!UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        return; // If the streaming was turned off, we don't have to do anything
    }

    // 0 means 'unknown': treat the whole stream until a short packet as one payload
    const size_t max_payload = uvc_stream->dynamic.dwMaxPayloadTransferSize ?
                               uvc_stream->dynamic.dwMaxPayloadTransferSize : SIZE_MAX;
    const uint8_t *p = transfer->data_buffer;
    size_t left = transfer->actual_num_bytes;
    const bool short_transfer = transfer->actual_num_bytes < transfer->num_bytes;

    while (left > 0) {
        if (uvc_stream->single_thread.bulk_payload_left == 0) {
            // Start of a payload: parse and strip its header
            const uvc_payload_header_t *hdr = (const uvc_payload_header_t *)p;
            if (!uvc_frame_payload_header_validate(hdr, left)) {
                ESP_LOGD(TAG, "invalid UVC payload header %02x %02x, len %u", p[0], left > 1 ? p[1] : 0, (unsigned)left);
                // Lost sync: drop the current frame and the rest of this transfer.
                // A short packet (end of a frame's last payload) re-synchronizes us.
                uvc_stream->single_thread.skip_current_frame = true;
                break;
            }
            const uint8_t hdr_len = hdr->bHeaderLength;
            const uint8_t fid = hdr->bmHeaderInfo.frame_id;

            // FID toggle marks a new frame, even for devices that never set the EOF bit
            if (uvc_stream->single_thread.bulk_in_frame && fid != uvc_stream->single_thread.current_frame_id) {
                bulk_end_frame(uvc_stream);
            }
            uvc_stream->single_thread.current_frame_id = fid;
            uvc_stream->single_thread.bulk_payload_eof = hdr->bmHeaderInfo.end_of_frame;
            uvc_stream->single_thread.bulk_payload_left = (max_payload > hdr_len) ? max_payload - hdr_len : 1;
            uvc_stream->single_thread.bulk_payload_err = hdr->bmHeaderInfo.error;
            if (hdr->bmHeaderInfo.error) {
                ESP_LOGW(TAG, "frame error flag set");
                uvc_stream->single_thread.skip_current_frame = true;
            }
            p += hdr_len;
            left -= hdr_len;
            if (left == 0) {
                break;
            }
        }

        size_t take = left < uvc_stream->single_thread.bulk_payload_left ? left : uvc_stream->single_thread.bulk_payload_left;
        if (!uvc_stream->single_thread.bulk_in_frame) {
            bulk_start_frame(uvc_stream, p, take);
            if (uvc_stream->single_thread.bulk_payload_err) {
                uvc_stream->single_thread.skip_current_frame = true;
            }
        }
        bulk_add_frame_data(uvc_stream, p, take);
        p += take;
        left -= take;
        uvc_stream->single_thread.bulk_payload_left -= take;

        if (uvc_stream->single_thread.bulk_payload_left == 0 && uvc_stream->single_thread.bulk_payload_eof) {
            bulk_end_frame(uvc_stream);
        }
    }

    // A short packet always terminates the current payload
    if (short_transfer) {
        if (uvc_stream->single_thread.bulk_payload_eof) {
            bulk_end_frame(uvc_stream);
        }
        uvc_stream->single_thread.bulk_payload_left = 0;
        uvc_stream->single_thread.bulk_payload_eof = false;
        uvc_stream->single_thread.bulk_payload_err = false;
    }

    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        usb_host_transfer_submit(transfer); // Restart the transfer
    }
}
