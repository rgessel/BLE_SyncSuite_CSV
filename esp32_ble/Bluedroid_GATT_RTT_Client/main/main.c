/*
 * BLE GATT central: scans for "ESP_RTT_RX", connects, enables NOTIFY on the RTT
 * characteristic, then periodically writes 8 bytes and logs round-trip time when
 * the notification returns.
 *
 * This measures application-level RTT (connection interval + stack), not RF ToF.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#define RTT_PEER_NAME "ESP_RTT_RX"
#define PING_WRITE_LEN   8
#define PING_NOTIFY_LEN  16

static const uint8_t rtt_svc_uuid128[ESP_UUID_LEN_128] = {
    0x24, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0xA1, 0xA1, 0x16, 0x00,
};

static const uint8_t rtt_chr_uuid128[ESP_UUID_LEN_128] = {
    0x25, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0xA1, 0xA1, 0x16, 0x00,
};

static const char *TAG = "RTT_CL";

static esp_gatt_if_t g_gattc_if = ESP_GATT_IF_NONE;
static uint16_t g_conn_id = 0xFFFF;
static esp_bd_addr_t g_remote_bda;
static esp_ble_addr_type_t g_remote_addr_type = BLE_ADDR_TYPE_PUBLIC;
static bool g_scanning = false;
static bool g_connected = false;
static bool g_connect_pending = false;

static uint16_t g_svc_start = 0;
static uint16_t g_svc_end = 0;
static uint16_t g_char_handle = 0;
static uint16_t g_cccd_handle = 0;
static bool g_service_resolved = false;
static bool g_cccd_done = false;

static uint64_t g_t_send_us = 0;

static esp_ble_scan_params_t scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

static bool uuid128_eq(const esp_bt_uuid_t *u, const uint8_t *v128)
{
    return u->len == ESP_UUID_LEN_128 && memcmp(u->uuid.uuid128, v128, ESP_UUID_LEN_128) == 0;
}

static void start_scan_if_idle(void)
{
    if (g_scanning || g_connected) {
        return;
    }
    g_service_resolved = false;
    g_cccd_done = false;
    g_char_handle = 0;
    g_cccd_handle = 0;
    g_svc_start = 0;
    g_svc_end = 0;

    esp_err_t err = esp_ble_gap_set_scan_params(&scan_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params: %s", esp_err_to_name(err));
    }
}

static void request_conn_params(const esp_bd_addr_t bda)
{
    esp_ble_conn_update_params_t conn = {0};
    memcpy(conn.bda, bda, sizeof(esp_bd_addr_t));
    conn.min_int = 0x06;
    conn.max_int = 0x0C;
    conn.latency = 0;
    conn.timeout = 400;
    esp_err_t err = esp_ble_gap_update_conn_params(&conn);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "update_conn_params: %s", esp_err_to_name(err));
    }
}

static void discover_next(esp_gatt_if_t gattc_if)
{
    if (!g_connected || g_gattc_if != gattc_if) {
        return;
    }

    if (!g_service_resolved) {
        esp_err_t err = esp_ble_gattc_search_service(gattc_if, g_conn_id, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "search_service: %s", esp_err_to_name(err));
        }
        return;
    }

    if (g_char_handle == 0) {
        esp_bt_uuid_t chr_uuid = {0};
        chr_uuid.len = ESP_UUID_LEN_128;
        memcpy(chr_uuid.uuid.uuid128, rtt_chr_uuid128, ESP_UUID_LEN_128);

        esp_gattc_char_elem_t ch;
        uint16_t count = 1;
        esp_gatt_status_t st = esp_ble_gattc_get_char_by_uuid(
            gattc_if,
            g_conn_id,
            g_svc_start,
            g_svc_end,
            chr_uuid,
            &ch,
            &count);
        if (st != ESP_GATT_OK || count == 0) {
            ESP_LOGE(TAG, "get_char_by_uuid status=%d count=%u", st, count);
            return;
        }
        g_char_handle = ch.char_handle;
        ESP_LOGI(TAG, "Char handle=%u", g_char_handle);
    }

    if (g_cccd_handle == 0) {
        esp_bt_uuid_t descr_uuid = {0};
        descr_uuid.len = ESP_UUID_LEN_16;
        descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

        esp_gattc_descr_elem_t de;
        uint16_t count = 1;
        esp_gatt_status_t st = esp_ble_gattc_get_descr_by_char_handle(
            gattc_if,
            g_conn_id,
            g_char_handle,
            descr_uuid,
            &de,
            &count);
        if (st != ESP_GATT_OK || count == 0) {
            ESP_LOGE(TAG, "get_descr_by_char_handle status=%d count=%u", st, count);
            return;
        }
        g_cccd_handle = de.handle;
        ESP_LOGI(TAG, "CCCD handle=%u", g_cccd_handle);
    }

    if (!g_cccd_done && g_cccd_handle != 0) {
        uint8_t val[] = {0x01, 0x00};
        esp_err_t err = esp_ble_gattc_write_char_descr(
            gattc_if,
            g_conn_id,
            g_cccd_handle,
            sizeof(val),
            val,
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write_char_descr: %s", esp_err_to_name(err));
        }
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "scan_param_set status=%d", param->scan_param_cmpl.status);
            break;
        }
        esp_err_t err = esp_ble_gap_start_scanning(30);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start_scanning: %s", esp_err_to_name(err));
        } else {
            g_scanning = true;
            ESP_LOGI(TAG, "Scanning for %s ...", RTT_PEER_NAME);
        }
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "scan_start status=%d", param->scan_start_cmpl.status);
            g_scanning = false;
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
            break;
        }
        uint8_t adv_name_len = 0;
        uint8_t *name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                 ESP_BLE_AD_TYPE_NAME_CMPL,
                                                 &adv_name_len);
        if (name == NULL || adv_name_len == 0) {
            break;
        }
        if (adv_name_len != strlen(RTT_PEER_NAME) || strncmp((char *)name, RTT_PEER_NAME, adv_name_len) != 0) {
            break;
        }

        ESP_LOGI(TAG, "Found peer, connecting...");
        g_connect_pending = true;
        memcpy(g_remote_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
        g_remote_addr_type = param->scan_rst.ble_addr_type;

        if (g_gattc_if == ESP_GATT_IF_NONE) {
            ESP_LOGW(TAG, "GATTC not registered yet");
            g_connect_pending = false;
            break;
        }
        esp_ble_gap_stop_scanning();
        esp_err_t oerr = esp_ble_gattc_open(g_gattc_if, g_remote_bda, g_remote_addr_type, true);
        if (oerr != ESP_OK) {
            ESP_LOGE(TAG, "gattc_open: %s", esp_err_to_name(oerr));
            g_connect_pending = false;
            start_scan_if_idle();
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        g_scanning = false;
        if (!g_connect_pending && !g_connected) {
            start_scan_if_idle();
        }
        break;

    default:
        break;
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "REG_EVT status=%d if=%d", param->reg.status, gattc_if);
        if (param->reg.status != ESP_GATT_OK) {
            break;
        }
        g_gattc_if = gattc_if;
        start_scan_if_idle();
        break;

    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "CONNECT_EVT conn_id=%u if=%d", param->connect.conn_id, gattc_if);
        g_connect_pending = false;
        g_conn_id = param->connect.conn_id;
        g_connected = true;
        g_gattc_if = gattc_if;
        memcpy(g_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        esp_ble_gattc_mtu_req(gattc_if, g_conn_id, 247);
        request_conn_params(param->connect.remote_bda);
        discover_next(gattc_if);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "OPEN_EVT status=%d", param->open.status);
            g_connect_pending = false;
            g_connected = false;
            start_scan_if_idle();
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        esp_gatt_srvc_id_t *sid = &param->search_res.srvc_id;
        if (uuid128_eq(&sid->id.uuid, rtt_svc_uuid128)) {
            g_svc_start = param->search_res.start_handle;
            g_svc_end = param->search_res.end_handle;
            ESP_LOGI(TAG, "RTT service handles %u..%u", g_svc_start, g_svc_end);
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "SEARCH_CMPL status=%d", param->search_cmpl.status);
            break;
        }
        if (g_svc_start == 0 || g_svc_end == 0) {
            ESP_LOGE(TAG, "RTT service not found");
            break;
        }
        g_service_resolved = true;
        discover_next(gattc_if);
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "WRITE_DESCR status=%d", param->write.status);
            break;
        }
        g_cccd_done = true;
        ESP_LOGI(TAG, "Notifications enabled; RTT pings active");
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "MTU %u", param->cfg_mtu.mtu);
        }
        break;

    case ESP_GATTC_NOTIFY_EVT: {
        if (param->notify.handle != g_char_handle || param->notify.value_len < PING_NOTIFY_LEN) {
            break;
        }
        uint64_t now = (uint64_t)esp_timer_get_time();
        if (g_t_send_us > 0) {
            int64_t rtt_us = (int64_t)(now - g_t_send_us);
            uint64_t echo = 0;
            for (int i = 0; i < 8; i++) {
                echo |= (uint64_t)param->notify.value[i] << (8 * i);
            }
            uint64_t srv_rx = 0;
            for (int i = 0; i < 8; i++) {
                srv_rx |= (uint64_t)param->notify.value[8 + i] << (8 * i);
            }
            ESP_LOGI(TAG, "RTT ~ %" PRId64 " us (echo_t0=%" PRIu64 " us, srv_rx=%" PRIu64 " us)",
                     rtt_us, echo, srv_rx);
        }
        break;
    }

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "DISCONNECT reason=0x%02x", param->disconnect.reason);
        g_connected = false;
        g_conn_id = 0xFFFF;
        g_service_resolved = false;
        g_cccd_done = false;
        g_char_handle = 0;
        g_cccd_handle = 0;
        g_svc_start = 0;
        g_svc_end = 0;
        start_scan_if_idle();
        break;

    default:
        break;
    }
}

static void rtt_ping_task(void *arg)
{
    uint8_t buf[PING_WRITE_LEN];

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (!g_connected || !g_cccd_done || g_char_handle == 0 || g_gattc_if == ESP_GATT_IF_NONE) {
            continue;
        }

        uint64_t stamp = (uint64_t)esp_timer_get_time();
        for (int i = 0; i < PING_WRITE_LEN; i++) {
            buf[i] = (uint8_t)(stamp >> (8 * i));
        }

        g_t_send_us = (uint64_t)esp_timer_get_time();
        esp_err_t err = esp_ble_gattc_write_char(
            g_gattc_if,
            g_conn_id,
            g_char_handle,
            PING_WRITE_LEN,
            buf,
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write_char: %s", esp_err_to_name(err));
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));

    ret = esp_ble_gatt_set_local_mtu(247);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set_local_mtu: %s", esp_err_to_name(ret));
    }

    xTaskCreate(rtt_ping_task, "rtt_ping", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "RTT client started");
}
