/*
 * Bluetooth LE link to the Blackmagic camera. See cam_ble.h.
 *
 * Connection sequence (ported from MagicPilot_Remote, ble_connection.h): scan for the camera's
 * Camera Control service, open a GATT connection, request MITM encryption (the camera asks for
 * its PIN the first time), search the service, write our name, then enable notifications on the
 * incoming control, timecode and status characteristics one after the other. Every notification
 * goes to cam_handle_packets(). A control task drives retries and sends queued packets one write
 * at a time.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_common_api.h"
#include "esp_hosted.h"
#include "esp_hosted_bluedroid.h"
#include "bsp/esp-bsp.h"
#include "cam.h"
#include "cam_ble.h"

static const char *TAG = "cam_ble";

#define SCAN_SECONDS        8
#define RETRY_DELAY_MS      2000
#define CANCEL_DELAY_MS     30000       /* after the PIN entry was cancelled */
#define WRITE_TIMEOUT_US    2000000
#define REMOTE_NAME         "MagicPilot Live"

/* Blackmagic Camera Control service and characteristics */
static const char *SERVICE_UUID = "291D567A-6D75-11E6-8B77-86F30CA893D3";
static const char *OUTGOING_UUID = "5DD3465F-1AEE-4299-8493-D2ECA2F8E1BB";    /* control packets we send */
static const char *INCOMING_UUID = "B864E140-76A0-416A-BF30-5876504537D9";    /* control packets the camera reports */
static const char *TIMECODE_UUID = "6D8F2110-86F1-41BF-9AFB-451D87E976C8";
static const char *STATUS_UUID = "7FE8691D-95DC-4FC5-8ABD-CA74339B51B9";
static const char *DEVICE_NAME_UUID = "FFAC0C52-C9FB-41A0-B063-CC76282EB89C";   /* the name the camera shows for us */

/* Control task events */
#define EV_RETRY        (1u << 0)   /* scan again after a delay */
#define EV_SEND         (1u << 1)   /* a packet was queued or a write completed */
#define EV_FORGET       (1u << 2)   /* remove every stored pairing */

static TaskHandle_t s_task;
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id;
static esp_bd_addr_t s_peer;
static esp_bd_addr_t s_pin_peer;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_out_handle, s_name_handle;
static uint16_t s_notify_handle[3];
static esp_gatt_char_prop_t s_notify_prop[3];
static int s_notify_index;
static uint16_t s_pending_cccd;
static volatile bool s_ready;               /* service found and the control characteristic is writable */
static volatile bool s_busy;                /* a scan or connection attempt is in flight */
static volatile bool s_found;
static volatile bool s_open, s_secured, s_searching;
static volatile bool s_stack_ok;
static volatile bool s_cancelled;           /* PIN entry cancelled: wait longer before retrying */
static char s_cp_version[16];
static esp_bd_addr_t s_bonded[4];           /* paired cameras, refreshed before every scan */
static int s_bonded_n;
static esp_bd_addr_t s_skip[4];             /* matched, but turned out to have no camera service: the last 4 */
static unsigned s_skip_n;                   /* added so far */

/* ---- Write queue ---- */

typedef struct {
    uint8_t data[64];           /* the protocol's largest packet */
    uint8_t len;
    uint16_t key;
} pkt_t;

#define QUEUE_LEN 16
static pkt_t s_queue[QUEUE_LEN];
static int s_q_head, s_q_count;
static bool s_in_flight;
static int64_t s_in_flight_t;
static SemaphoreHandle_t s_q_lock;

/* ---- UUID helpers ---- */

/* Value of a hex digit, -1 if c is not one */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* UUID string to the reversed byte order Bluedroid uses */
static esp_bt_uuid_t uuid128(const char *text)
{
    esp_bt_uuid_t uuid = { 0 };
    uint8_t bytes[16];
    int n = 0;
    for (const char *p = text; *p && n < 16;) {
        if (*p == '-') {
            p++;
            continue;
        }
        const int hi = hex_nibble(p[0]), lo = hex_nibble(p[1]);
        if (hi < 0 || lo < 0) {
            return uuid;
        }
        bytes[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    if (n != 16) {
        return uuid;
    }
    uuid.len = ESP_UUID_LEN_128;
    for (int i = 0; i < 16; i++) {
        uuid.uuid.uuid128[i] = bytes[15 - i];
    }
    return uuid;
}

/* u is the 128-bit UUID written as text */
static bool uuid_is(const esp_bt_uuid_t *u, const char *text)
{
    const esp_bt_uuid_t e = uuid128(text);
    return u->len == ESP_UUID_LEN_128 && memcmp(u->uuid.uuid128, e.uuid.uuid128, 16) == 0;
}

/* Advertised name (complete, else shortened) into name; "" without one */
static void adv_name(uint8_t *adv, uint8_t adv_len, char *name, size_t size)
{
    uint8_t len = 0;
    uint8_t *p = esp_ble_resolve_adv_data_by_type(adv, adv_len, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
    if (!p) {
        p = esp_ble_resolve_adv_data_by_type(adv, adv_len, ESP_BLE_AD_TYPE_NAME_SHORT, &len);
    }
    memset(name, 0, size);
    if (p) {
        memcpy(name, p, len < size - 1 ? len : size - 1);
    }
}

/* A camera advertises the Camera Control service, a Blackmagic name, or both */
static bool is_camera(uint8_t *adv, uint8_t adv_len, const char *name)
{
    if (strstr(name, "Blackmagic") || strstr(name, "BMPCC")) {
        return true;
    }
    uint8_t len = 0;
    const esp_bt_uuid_t svc = uuid128(SERVICE_UUID);
    const uint8_t types[2] = { ESP_BLE_AD_TYPE_128SRV_CMPL, ESP_BLE_AD_TYPE_128SRV_PART };
    for (int t = 0; t < 2; t++) {
        uint8_t *u = esp_ble_resolve_adv_data_by_type(adv, adv_len, types[t], &len);
        for (int off = 0; u && off + 16 <= len; off += 16) {
            if (memcmp(u + off, svc.uuid.uuid128, 16) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Static: the list is large and Bluedroid callbacks run on a small stack */
static esp_ble_bond_dev_t s_bond_list[4];

/* Reads the stored pairings into s_bonded, 4 at most */
static void refresh_bonds(void)
{
    int n = esp_ble_get_bond_device_num();
    n = n < 0 ? 0 : (n > 4 ? 4 : n);
    if (n && esp_ble_get_bond_device_list(&n, s_bond_list) != ESP_OK) {
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        memcpy(s_bonded[i], s_bond_list[i].bd_addr, sizeof(esp_bd_addr_t));
    }
    s_bonded_n = n;
}

/* addr is one of the first n of list */
static bool addr_in(esp_bd_addr_t *list, int n, const esp_bd_addr_t addr)
{
    for (int i = 0; i < n; i++) {
        if (memcmp(list[i], addr, sizeof(esp_bd_addr_t)) == 0) {
            return true;
        }
    }
    return false;
}

/* addr is a paired device (as of the last refresh_bonds) */
static bool is_bonded(const esp_bd_addr_t addr)
{
    return addr_in(s_bonded, s_bonded_n, addr);
}

/* ---- Connection lifecycle ---- */

/* Ends this attempt with status for the user; the control task scans again after a delay */
static void retry_later(const char *status)
{
    s_busy = false;
    cam_set_link(CAM_LINK_IDLE, status);
    xTaskNotify(s_task, EV_RETRY, eSetBits);
}

/* Scans SCAN_SECONDS for the camera, unless an attempt is in flight, the link is up or the stack is not ready */
static void start_scan(void)
{
    if (s_busy || s_ready || !s_stack_ok) {
        return;
    }
    s_busy = true;
    s_found = false;
    s_open = s_secured = s_searching = false;
    s_svc_start = s_svc_end = 0;
    refresh_bonds();
    cam_set_link(CAM_LINK_SCANNING, "Searching for camera");
    if (esp_ble_gap_start_scanning(SCAN_SECONDS) != ESP_OK) {
        retry_later("Bluetooth scan failed");
    }
}

/* Handle of a characteristic of the camera service, 0 if missing; its properties into props unless NULL */
static uint16_t find_char(const char *uuid_text, esp_gatt_char_prop_t *props)
{
    esp_gattc_char_elem_t ch = { 0 };
    uint16_t count = 1;
    const esp_bt_uuid_t uuid = uuid128(uuid_text);
    if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id, s_svc_start, s_svc_end, uuid, &ch, &count) != ESP_GATT_OK ||
            count == 0) {
        return 0;
    }
    if (props) {
        *props = ch.properties;
    }
    return ch.char_handle;
}

/* Starts subscribing to the next characteristic that can notify (from s_notify_index); its events call this again */
static void next_notification(void)
{
    while (s_notify_index < 3) {
        const uint16_t h = s_notify_handle[s_notify_index];
        const uint8_t props = s_notify_prop[s_notify_index];
        if (h && (props & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE))) {
            if (esp_ble_gattc_register_for_notify(s_gattc_if, s_peer, h) == ESP_OK) {
                return;
            }
            ESP_LOGW(TAG, "Notification registration failed for handle %u", h);
        }
        s_notify_index++;
    }
    ESP_LOGI(TAG, "Notifications enabled");
}

/* Subscribes by writing the characteristic's client configuration descriptor */
static bool write_cccd(uint16_t char_handle)
{
    esp_bt_uuid_t cccd = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG };
    esp_gattc_descr_elem_t descr = { 0 };
    uint16_t count = 1;
    if (esp_ble_gattc_get_descr_by_char_handle(s_gattc_if, s_conn_id, char_handle, cccd, &descr, &count) != ESP_GATT_OK ||
            count == 0) {
        return false;
    }
    uint8_t value[2] = { (s_notify_prop[s_notify_index] & ESP_GATT_CHAR_PROP_BIT_NOTIFY) ? 1 : 2, 0 };
    s_pending_cccd = descr.handle;
    if (esp_ble_gattc_write_char_descr(s_gattc_if, s_conn_id, descr.handle, sizeof(value), value,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM) == ESP_OK) {
        return true;
    }
    s_pending_cccd = 0;
    return false;
}

/* Service found: looks up its characteristics, writes our name, takes the link as ready and subscribes */
static void service_found(void)
{
    esp_gatt_char_prop_t out_props = 0;
    s_notify_handle[0] = find_char(INCOMING_UUID, &s_notify_prop[0]);
    s_notify_handle[1] = find_char(TIMECODE_UUID, &s_notify_prop[1]);
    s_notify_handle[2] = find_char(STATUS_UUID, &s_notify_prop[2]);
    s_out_handle = find_char(OUTGOING_UUID, &out_props);
    s_name_handle = find_char(DEVICE_NAME_UUID, NULL);
    ESP_LOGI(TAG, "Characteristics: out=%u in=%u timecode=%u status=%u name=%u", s_out_handle,
             s_notify_handle[0], s_notify_handle[1], s_notify_handle[2], s_name_handle);
    if (!s_out_handle || !(out_props & ESP_GATT_CHAR_PROP_BIT_WRITE)) {
        /* Nothing to control it with: close the link, which starts the search again */
        cam_set_link(CAM_LINK_CONNECTING, "Camera control unavailable");
        esp_ble_gap_disconnect(s_peer);
        return;
    }
    if (s_name_handle) {
        static uint8_t name[] = REMOTE_NAME;
        esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_name_handle, sizeof(name) - 1, name,
                                 ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
    }
    s_ready = true;
    s_busy = false;
    cam_reset_values();
    cam_set_link(CAM_LINK_CONNECTED, "Connected");
    esp_ble_gattc_send_mtu_req(s_gattc_if, s_conn_id);
    s_notify_index = 0;
    next_notification();
}

/* Searches the camera service once the link is both open and encrypted, which complete in either order */
static void search_if_ready(void)
{
    if (!s_open || !s_secured || s_searching) {
        return;
    }
    esp_bt_uuid_t svc = uuid128(SERVICE_UUID);
    s_searching = true;
    if (esp_ble_gattc_search_service(s_gattc_if, s_conn_id, &svc) != ESP_OK) {
        esp_ble_gap_disconnect(s_peer);
    }
}

/* After a disconnect: clears the link and the send queue, forgets the camera's values and tries again later */
static void link_lost(void)
{
    const bool was_ready = s_ready;
    s_ready = false;
    s_open = s_secured = s_searching = false;
    s_out_handle = s_name_handle = s_pending_cccd = 0;
    memset(s_notify_handle, 0, sizeof(s_notify_handle));
    xSemaphoreTake(s_q_lock, portMAX_DELAY);
    s_q_count = 0;
    s_in_flight = false;
    xSemaphoreGive(s_q_lock);
    cam_reset_values();
    retry_later(s_cancelled ? "Pairing cancelled" : was_ready ? "Camera disconnected" : "Connection lost");
}

/* ---- GAP events: scan results and pairing ---- */

/* GAP events, in Bluedroid's task: scan results, the camera's security and PIN requests, the pairing result */
static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (p->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_stack_ok = true;
            start_scan();
        } else {
            cam_set_link(CAM_LINK_DOWN, "Bluetooth scan setup failed");
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        /* Only while looking: results still come in after a camera was picked and the scan told to stop */
        if (!s_busy || s_found) {
            break;
        }
        if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            char name[32];
            /* ble_adv holds the advertising data and then the scan response: the name may be in either */
            const uint8_t adv_len = p->scan_rst.adv_data_len + p->scan_rst.scan_rsp_len;
            adv_name(p->scan_rst.ble_adv, adv_len, name, sizeof(name));
            if (!is_camera(p->scan_rst.ble_adv, adv_len, name) && !is_bonded(p->scan_rst.bda)) {
                break;
            }
            if (addr_in(s_skip, s_skip_n < 4 ? (int)s_skip_n : 4, p->scan_rst.bda)) {
                break;
            }
            s_found = true;
            memcpy(s_peer, p->scan_rst.bda, sizeof(s_peer));
            ESP_LOGI(TAG, "Camera %02X:%02X:%02X:%02X:%02X:%02X \"%s\" rssi %d%s", s_peer[0], s_peer[1], s_peer[2],
                     s_peer[3], s_peer[4], s_peer[5], name, p->scan_rst.rssi, is_bonded(s_peer) ? " (paired)" : "");
            cam_set_name(name);
            cam_set_link(CAM_LINK_CONNECTING, "Connecting to camera");
            esp_ble_gap_stop_scanning();
            if (esp_ble_gattc_open(s_gattc_if, s_peer, p->scan_rst.ble_addr_type, true) != ESP_OK) {
                retry_later("Connection failed");
            }
        } else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            retry_later("Camera not found - Bluetooth on?");
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        ESP_LOGI(TAG, "Camera asks for its PIN");
        /* The answer goes to the device that asked, once the PIN is typed in */
        memcpy(s_pin_peer, p->ble_security.ble_req.bd_addr, sizeof(s_pin_peer));
        cam_set_link(CAM_LINK_PIN, "Enter the PIN shown on the camera");
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (p->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "Encrypted link ready");
            s_secured = true;
            cam_set_link(CAM_LINK_CONNECTING, "Paired, reading camera");
            search_if_ready();
        } else {
            const uint8_t reason = p->ble_security.auth_cmpl.fail_reason;
            ESP_LOGW(TAG, "Pairing failed, reason 0x%02x", reason);
            /*
             * A stale bond (the camera forgot us) makes every retry fail: pair from scratch. Timeouts
             * (link lost during the handshake) keep the bond.
             */
            const bool timeout = reason == ESP_AUTH_SMP_CONN_TOUT || reason == ESP_AUTH_SMP_RSP_TIMEOUT;
            if (!s_cancelled && !timeout && is_bonded(p->ble_security.auth_cmpl.bd_addr)) {
                ESP_LOGW(TAG, "Removing the stored pairing, the camera will ask for its PIN again");
                esp_ble_remove_bond_device(p->ble_security.auth_cmpl.bd_addr);
            }
            cam_set_link(CAM_LINK_CONNECTING, s_cancelled ? "Pairing cancelled" : "Pairing failed");
            esp_ble_gap_disconnect(p->ble_security.auth_cmpl.bd_addr);
        }
        break;
    default:
        break;
    }
}

/* ---- GATT client events ---- */

/* GATT client events, in Bluedroid's task: connection, service search, subscriptions, writes and notifications */
static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *p)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (p->reg.status != ESP_GATT_OK) {
            cam_set_link(CAM_LINK_DOWN, "Bluetooth registration failed");
            return;
        }
        s_gattc_if = gattc_if;
        /* The first scan starts in the GAP callback once these are set */
        static esp_ble_scan_params_t scan = {
            .scan_type = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval = 0x50,
            .scan_window = 0x30,
            .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
        };
        esp_ble_gap_set_scan_params(&scan);
        return;
    }
    if (gattc_if != s_gattc_if) {
        return;
    }
    switch (event) {
    case ESP_GATTC_CONNECT_EVT:
        s_conn_id = p->connect.conn_id;
        memcpy(s_peer, p->connect.remote_bda, sizeof(s_peer));
        /* The camera's characteristics need an encrypted link: pair (or use the stored bond) right away */
        if (esp_ble_set_encryption(s_peer, ESP_BLE_SEC_ENCRYPT_MITM) != ESP_OK) {
            esp_ble_gap_disconnect(s_peer);
        }
        break;
    case ESP_GATTC_OPEN_EVT:
        if (p->open.status != ESP_GATT_OK) {
            retry_later("Connection failed");
            break;
        }
        s_conn_id = p->open.conn_id;
        s_open = true;
        search_if_ready();
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if (uuid_is(&p->search_res.srvc_id.uuid, SERVICE_UUID)) {
            s_svc_start = p->search_res.start_handle;
            s_svc_end = p->search_res.end_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (p->search_cmpl.status != ESP_GATT_OK || s_svc_start == 0) {
            if (p->search_cmpl.status == ESP_GATT_OK) {
                /* Not a camera after all: ignore it in later scans */
                memcpy(s_skip[s_skip_n++ % 4], s_peer, sizeof(esp_bd_addr_t));
            }
            cam_set_link(CAM_LINK_CONNECTING, "Camera service not found");
            esp_ble_gap_disconnect(s_peer);
            break;
        }
        service_found();
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        /* Registered on our side; the camera only notifies once the CCCD is written. On failure, the next one */
        const bool ok = p->reg_for_notify.status == ESP_GATT_OK && s_notify_index < 3 &&
                        p->reg_for_notify.handle == s_notify_handle[s_notify_index];
        if (!ok || !write_cccd(p->reg_for_notify.handle)) {
            s_notify_index++;
            next_notification();
        }
        break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT:
        /* That CCCD write is done, successful or not: on to the next characteristic */
        if (s_pending_cccd && p->write.handle == s_pending_cccd) {
            s_pending_cccd = 0;
            s_notify_index++;
            next_notification();
        }
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "MTU %d", p->cfg_mtu.mtu);
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
        /* The answer to a queued packet (the name write is answered too, but not queued): the next may go */
        if (p->write.handle == s_out_handle) {
            if (p->write.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "Write failed: 0x%x", p->write.status);
            }
            xSemaphoreTake(s_q_lock, portMAX_DELAY);
            s_in_flight = false;
            xSemaphoreGive(s_q_lock);
            xTaskNotify(s_task, EV_SEND, eSetBits);
        }
        break;
    case ESP_GATTC_NOTIFY_EVT: {
        static int traced;
        const uint8_t *d = p->notify.value;
        const size_t n = p->notify.value_len;
        /* Trace what the camera reports, apart from the timecode and the battery report (every second) */
        const bool periodic = n >= 8 && d[0] == 0xFF && d[4] == 9 && (d[5] == 4 || d[5] == 0);
        if (!periodic && traced < 300) {
            char hex[3 * 24 + 1] = { 0 };
            for (size_t i = 0; i < n && i < 24; i++) {
                snprintf(hex + i * 3, 4, " %02X", d[i]);
            }
            ESP_LOGI(TAG, "notify %u:%s", p->notify.handle, hex);
            traced++;
        }
        cam_handle_packets(d, n);
        break;
    }
    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Disconnected, reason 0x%02x", (unsigned)p->disconnect.reason);
        link_lost();
        break;
    default:
        break;
    }
}

/* ---- Write queue ---- */

bool cam_ble_send(const uint8_t *packet, size_t len, uint16_t key)
{
    if (!s_ready || len > sizeof(s_queue[0].data)) {
        return false;
    }
    xSemaphoreTake(s_q_lock, portMAX_DELAY);
    bool merged = false;
    if (key) {
        /* Replace a queued (not yet written) packet with the same key */
        for (int i = 0; i < s_q_count; i++) {
            pkt_t *q = &s_queue[(s_q_head + i) % QUEUE_LEN];
            if (q->key == key) {
                if ((key & CAM_BLE_KEY_SUM) && len >= 10 && q->len >= 10) {
                    int32_t sum = (int16_t)(q->data[8] | (q->data[9] << 8)) + (int16_t)(packet[8] | (packet[9] << 8));
                    sum = sum < INT16_MIN ? INT16_MIN : (sum > INT16_MAX ? INT16_MAX : sum);
                    q->data[8] = (uint8_t)sum;
                    q->data[9] = (uint8_t)((uint32_t)sum >> 8);
                } else {
                    memcpy(q->data, packet, len);
                    q->len = (uint8_t)len;
                }
                merged = true;
                break;
            }
        }
    }
    if (!merged) {
        if (s_q_count == QUEUE_LEN) {
            ESP_LOGW(TAG, "Send queue full, packet dropped");
            xSemaphoreGive(s_q_lock);
            return false;
        }
        pkt_t *q = &s_queue[(s_q_head + s_q_count) % QUEUE_LEN];
        memcpy(q->data, packet, len);
        q->len = (uint8_t)len;
        q->key = key;
        s_q_count++;
    }
    xSemaphoreGive(s_q_lock);
    xTaskNotify(s_task, EV_SEND, eSetBits);
    return true;
}

/* Sends the next queued packet if no write is waiting for its response */
static void pump_queue(void)
{
    xSemaphoreTake(s_q_lock, portMAX_DELAY);
    if (s_in_flight && esp_timer_get_time() - s_in_flight_t > WRITE_TIMEOUT_US) {
        ESP_LOGW(TAG, "Write response timeout");
        s_in_flight = false;
    }
    if (s_in_flight) {
        xSemaphoreGive(s_q_lock);
        return;
    }
    if (s_q_count && s_ready) {
        pkt_t *q = &s_queue[s_q_head];
        if (esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_out_handle, q->len, q->data,
                                     ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM) == ESP_OK) {
            s_in_flight = true;
            s_in_flight_t = esp_timer_get_time();
        }
        /* Either way the packet leaves the queue; a failed write is not retried */
        s_q_head = (s_q_head + 1) % QUEUE_LEN;
        s_q_count--;
    }
    xSemaphoreGive(s_q_lock);
}

/* ---- Pairing ---- */

void cam_ble_submit_pin(uint32_t pin)
{
    s_cancelled = false;
    cam_set_link(CAM_LINK_CONNECTING, "Pairing");
    esp_ble_passkey_reply(s_pin_peer, true, pin);
}

void cam_ble_cancel_pin(void)
{
    s_cancelled = true;
    esp_ble_passkey_reply(s_pin_peer, false, 0);
}

void cam_ble_forget(void)
{
    if (s_task) {
        xTaskNotify(s_task, EV_FORGET, eSetBits);
    }
}

/* Removes every stored pairing and drops the link if there is one: the camera then asks for its PIN again */
static void forget_all(void)
{
    int n = esp_ble_get_bond_device_num();
    if (n > 0) {
        esp_ble_bond_dev_t list[8];
        n = n > 8 ? 8 : n;
        if (esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                esp_ble_remove_bond_device(list[i].bd_addr);
            }
        }
    }
    ESP_LOGI(TAG, "Pairing removed");
    if (s_ready || s_open) {
        esp_ble_gap_disconnect(s_peer);
    }
}

const char *cam_ble_cp_version(void)
{
    return s_cp_version;
}

/* ---- Startup ---- */

/* Starts ESP-Hosted, the C6's controller and Bluedroid, and registers the callbacks; false (link down) on failure */
static bool bluetooth_init(void)
{
    /* Normally run by a constructor, which the PlatformIO link drops */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hosted_init());

    esp_hosted_coprocessor_fwver_t ver = { 0 };
    for (int attempt = 1;; attempt++) {
        esp_hosted_connect_to_slave();
        if (esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK) {
            break;
        }
        if (attempt == 3) {
            ESP_LOGE(TAG, "No link to the ESP32-C6 co-processor");
            cam_set_link(CAM_LINK_DOWN, "Bluetooth unavailable (no ESP32-C6)");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "ESP32-C6 runs ESP-Hosted %" PRIu32 ".%" PRIu32 ".%" PRIu32, ver.major1, ver.minor1, ver.patch1);
    snprintf(s_cp_version, sizeof(s_cp_version), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, ver.major1, ver.minor1, ver.patch1);

    /* Before 2.5.2 the co-processor starts its Bluetooth controller by itself and has no RPC for it */
    const uint32_t v = (ver.major1 << 16) | (ver.minor1 << 8) | ver.patch1;
    if (v >= 0x020502) {
        if (esp_hosted_bt_controller_init() != ESP_OK || esp_hosted_bt_controller_enable() != ESP_OK) {
            ESP_LOGW(TAG, "Could not start the Bluetooth controller on the co-processor");
        }
    }

    hosted_hci_bluedroid_open();
    esp_bluedroid_hci_driver_operations_t ops = {
        .send = hosted_hci_bluedroid_send,
        .check_send_available = hosted_hci_bluedroid_check_send_available,
        .register_host_callback = hosted_hci_bluedroid_register_host_callback,
    };
    esp_err_t err = esp_bluedroid_attach_hci_driver(&ops);
    if (err == ESP_OK) {
        err = esp_bluedroid_init();
    }
    if (err == ESP_OK) {
        err = esp_bluedroid_enable();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid start failed: %s", esp_err_to_name(err));
        cam_set_link(CAM_LINK_DOWN, "Bluetooth start failed");
        return false;
    }

    /* Keyboard-only I/O: the camera shows a PIN and we type it in */
    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_BOND_MITM;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_IN;
    uint8_t key_size = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));
    esp_ble_gatt_set_local_mtu(200);
    esp_ble_gap_set_device_name(REMOTE_NAME);
    esp_ble_gap_register_callback(gap_cb);
    esp_ble_gattc_register_callback(gattc_cb);
    esp_ble_gattc_app_register(0);
    ESP_LOGI(TAG, "Bluetooth ready, %d paired camera(s)", esp_ble_get_bond_device_num());
    return true;
}

/* Control task: starts Bluetooth, then serves the EV_* events; it wakes at least every 500 ms for the write timeout */
static void ble_task(void *arg)
{
    if (!bluetooth_init()) {
        vTaskDelete(NULL);
    }
    while (true) {
        uint32_t ev = 0;
        xTaskNotifyWait(0, UINT32_MAX, &ev, pdMS_TO_TICKS(500));
        if (ev & EV_FORGET) {
            forget_all();
        }
        if (ev & EV_RETRY) {
            vTaskDelay(pdMS_TO_TICKS(s_cancelled ? CANCEL_DELAY_MS : RETRY_DELAY_MS));
            s_cancelled = false;
            start_scan();
        }
        pump_queue();
    }
}

void cam_ble_start(void)
{
    /*
     * The C6 is powered through IO expander 1 (ESP-Hosted then resets it through GPIO 15). Done here,
     * in the caller's task: the BSP's expander access is not thread-safe and USB power is on the same
     * expander.
     */
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_feature_enable(BSP_FEATURE_WIFI, true));
    s_q_lock = xSemaphoreCreateMutex();
    assert(s_q_lock);
    xTaskCreatePinnedToCore(ble_task, "cam_ble", 6 * 1024, NULL, 3, &s_task, 0);
}
