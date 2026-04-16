/*
 * BLE time-of-flight (RTT) demo — two firmware images from one tree.
 *
 * Flash RECEIVER (menuconfig: Receiver) on 00:70:07:29:92:2E:
 *   - GATT peripheral, two primary services:
 *       1) RX: writable characteristic — central sends 12-byte packets; LED pulses on each write.
 *       2) Echo: notify characteristic — optional timestamp echo for logging on the central.
 *   - Rejects connections from addresses other than the transmitter MAC.
 *
 * Flash TRANSMITTER (menuconfig: Transmitter) on 00:70:07:29:92:36:
 *   - Scans, connects to receiver by public address, discovers services/characteristics.
 *   - Every 1 s: GATT write-with-response carrying seq + local tx time (µs).
 *   - Logs round-trip time (µs) from issue of write until ATT response (≈ 2× one-way
 *     over-the-air + stack; not raw RF ToF, but useful for relative delay measurements).
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#if CONFIG_TOF_ROLE_RECEIVER
#include "led_strip.h"
#endif

// ---------------------------------------------------------------------------
// Device addresses (ESP-IDF stores BD_ADDR with LSB first in byte[0])
// Compile only the constants needed for the selected role.
// ---------------------------------------------------------------------------
#if CONFIG_TOF_ROLE_TRANSMITTER
static const esp_bd_addr_t k_receiver_bda = {0x2E, 0x92, 0x29, 0x07, 0x70, 0x00};
#endif
#if CONFIG_TOF_ROLE_RECEIVER
static const esp_bd_addr_t k_transmitter_bda = {0x36, 0x92, 0x29, 0x07, 0x70, 0x00};
#endif

// 128-bit UUIDs (same byte order as other ESP-IDF 128-bit examples)
static const uint8_t tof_rx_svc_uuid128[16] = {
    0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF,
    0x10, 0x32, 0x54, 0x76, 0x00, 0x00, 0xA1, 0x01,
};
static const uint8_t tof_rx_chr_uuid128[16] = {
    0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF,
    0x10, 0x32, 0x54, 0x76, 0x00, 0x00, 0xA1, 0x02,
};
static const uint8_t tof_echo_svc_uuid128[16] = {
    0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF,
    0x10, 0x32, 0x54, 0x76, 0x00, 0x00, 0xA2, 0x01,
};
static const uint8_t tof_echo_chr_uuid128[16] = {
    0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF,
    0x10, 0x32, 0x54, 0x76, 0x00, 0x00, 0xA2, 0x02,
};

#define TOF_APP_ID 0
#define TOF_PAYLOAD_LEN 12
#define TOF_ECHO_PAYLOAD_LEN 20
#define TOF_WRITE_PERIOD_MS 1000
#define TOF_LED_PULSE_MS 120

#define RX_SVC_NUM_HANDLE 4
#define ECHO_SVC_NUM_HANDLE 6

#define ADV_CONFIG_FLAG (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)

static const char *TAG = "TOF_BLE";

#if CONFIG_TOF_ROLE_RECEIVER

// -------------------- Receiver: LED (WS2812) --------------------
#define LED_GPIO 2
#define LED_COUNT 1

static led_strip_handle_t s_strip;

static void led_init_rgb(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
    };

#if CONFIG_EXAMPLE_BLINK_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
#elif CONFIG_EXAMPLE_BLINK_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_cfg = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_cfg, &spi_cfg, &s_strip));
#else
#error "LED strip backend not set. Enable RMT or SPI backend in menuconfig."
#endif

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}

static void led_set_green(bool on)
{
    if (!s_strip) {
        return;
    }
    if (on) {
        led_strip_set_pixel(s_strip, 0, 0, 255, 0);
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
        led_strip_refresh(s_strip);
    }
}

static esp_timer_handle_t s_led_off_timer;

static void led_off_timer_cb(void *arg)
{
    (void)arg;
    led_set_green(false);
}

static void led_pulse_rx(void)
{
    led_set_green(true);
    if (s_led_off_timer) {
        esp_timer_stop(s_led_off_timer);
        esp_timer_start_once(s_led_off_timer, TOF_LED_PULSE_MS * 1000ULL);
    }
}

// -------------------- Receiver: GATT server state --------------------
static uint8_t s_adv_cfg_done;
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0xFFFF;

static uint16_t s_rx_char_hdl;
static uint16_t s_echo_char_hdl;
static uint16_t s_echo_cccd_hdl;
static bool s_echo_notify_enabled;

static uint8_t s_create_svc_count;
static uint8_t s_add_char_count;

static uint8_t s_rx_payload[TOF_PAYLOAD_LEN];
static uint8_t s_echo_payload[TOF_ECHO_PAYLOAD_LEN];

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = ESP_UUID_LEN_128,
    .p_service_uuid = tof_rx_svc_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_cfg_done &= (uint8_t)(~ADV_CONFIG_FLAG);
        if (s_adv_cfg_done == 0) {
            esp_ble_gap_start_advertising(&s_adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        s_adv_cfg_done &= (uint8_t)(~SCAN_RSP_CONFIG_FLAG);
        if (s_adv_cfg_done == 0) {
            esp_ble_gap_start_advertising(&s_adv_params);
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Adv start failed, status=%d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising started (receiver)");
        }
        break;
    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                              esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "GATTS REG_EVT");
        s_gatts_if = gatts_if;
        s_adv_cfg_done = ADV_CONFIG_FLAG;
        if (esp_ble_gap_config_adv_data(&s_adv_data)) {
            ESP_LOGE(TAG, "config adv data failed");
            break;
        }

        esp_gatt_srvc_id_t rx_id = {0};
        rx_id.is_primary = true;
        rx_id.id.inst_id = 0;
        rx_id.id.uuid.len = ESP_UUID_LEN_128;
        memcpy(rx_id.id.uuid.uuid.uuid128, tof_rx_svc_uuid128, 16);

        esp_ble_gatts_create_service(gatts_if, &rx_id, RX_SVC_NUM_HANDLE);
        break;

    case ESP_GATTS_CREATE_EVT:
        s_create_svc_count++;
        if (s_create_svc_count == 1) {
            ESP_LOGI(TAG, "RX service created, handle=%d", param->create.service_handle);
            esp_ble_gatts_start_service(param->create.service_handle);

            esp_bt_uuid_t cu = {0};
            cu.len = ESP_UUID_LEN_128;
            memcpy(cu.uuid.uuid128, tof_rx_chr_uuid128, 16);

            esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;

            esp_attr_value_t v = {
                .attr_max_len = TOF_PAYLOAD_LEN,
                .attr_len = TOF_PAYLOAD_LEN,
                .attr_value = s_rx_payload,
            };

            esp_ble_gatts_add_char(param->create.service_handle, &cu,
                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                   prop, &v, NULL);
        } else if (s_create_svc_count == 2) {
            ESP_LOGI(TAG, "Echo service created, handle=%d", param->create.service_handle);
            esp_ble_gatts_start_service(param->create.service_handle);

            esp_bt_uuid_t cu = {0};
            cu.len = ESP_UUID_LEN_128;
            memcpy(cu.uuid.uuid128, tof_echo_chr_uuid128, 16);

            esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

            esp_attr_value_t v = {
                .attr_max_len = TOF_ECHO_PAYLOAD_LEN,
                .attr_len = TOF_ECHO_PAYLOAD_LEN,
                .attr_value = s_echo_payload,
            };

            esp_ble_gatts_add_char(param->create.service_handle, &cu,
                                   ESP_GATT_PERM_READ,
                                   prop, &v, NULL);
        }
        break;

    case ESP_GATTS_ADD_CHAR_EVT:
        s_add_char_count++;
        if (s_add_char_count == 1) {
            s_rx_char_hdl = param->add_char.attr_handle;
            ESP_LOGI(TAG, "RX char handle=%d", s_rx_char_hdl);

            esp_gatt_srvc_id_t echo_id = {0};
            echo_id.is_primary = true;
            echo_id.id.inst_id = 0;
            echo_id.id.uuid.len = ESP_UUID_LEN_128;
            memcpy(echo_id.id.uuid.uuid.uuid128, tof_echo_svc_uuid128, 16);
            esp_ble_gatts_create_service(gatts_if, &echo_id, ECHO_SVC_NUM_HANDLE);
        } else if (s_add_char_count == 2) {
            s_echo_char_hdl = param->add_char.attr_handle;
            ESP_LOGI(TAG, "Echo char handle=%d", s_echo_char_hdl);

            esp_bt_uuid_t cccd = {0};
            cccd.len = ESP_UUID_LEN_16;
            cccd.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
            esp_ble_gatts_add_char_descr(param->add_char.service_handle, &cccd,
                                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                         NULL, NULL);
        }
        break;

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        s_echo_cccd_hdl = param->add_char_descr.attr_handle;
        ESP_LOGI(TAG, "Echo CCCD handle=%d — receiver ready", s_echo_cccd_hdl);
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_echo_cccd_hdl && param->write.len == 2) {
            uint16_t cfg = (uint16_t)(param->write.value[0] | (param->write.value[1] << 8));
            s_echo_notify_enabled = (cfg & 0x0001) != 0;
            ESP_LOGI(TAG, "Echo notify %s", s_echo_notify_enabled ? "enabled" : "disabled");
        } else if (param->write.handle == s_rx_char_hdl && param->write.len >= TOF_PAYLOAD_LEN) {
            memcpy(s_rx_payload, param->write.value, TOF_PAYLOAD_LEN);
            led_pulse_rx();

            if (s_echo_notify_enabled && s_gatts_if != ESP_GATT_IF_NONE && s_conn_id != 0xFFFF) {
                uint32_t seq = (uint32_t)s_rx_payload[0] | ((uint32_t)s_rx_payload[1] << 8) |
                               ((uint32_t)s_rx_payload[2] << 16) | ((uint32_t)s_rx_payload[3] << 24);
                uint64_t t_tx = 0;
                for (int i = 0; i < 8; i++) {
                    t_tx |= (uint64_t)s_rx_payload[4 + i] << (8 * i);
                }
                uint64_t t_rx = (uint64_t)esp_timer_get_time();

                memcpy(s_echo_payload, s_rx_payload, TOF_PAYLOAD_LEN);
                memcpy(s_echo_payload + TOF_PAYLOAD_LEN, &t_rx, sizeof(t_rx));

                (void)esp_ble_gatts_set_attr_value(s_echo_char_hdl, TOF_ECHO_PAYLOAD_LEN, s_echo_payload);
                esp_err_t ne = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_echo_char_hdl,
                                                           TOF_ECHO_PAYLOAD_LEN, s_echo_payload, false);
                if (ne != ESP_OK) {
                    ESP_LOGW(TAG, "echo notify failed: %s", esp_err_to_name(ne));
                } else {
                    ESP_LOGI(TAG, "RX seq=%" PRIu32 " t_tx_us=%" PRIu64 " t_rx_us=%" PRIu64,
                             seq, t_tx, t_rx);
                }
            }
        }

        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                        ESP_GATT_OK, NULL);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(TAG, "CONNECT conn_id=%u remote " ESP_BD_ADDR_STR,
                 param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        if (memcmp(param->connect.remote_bda, k_transmitter_bda, sizeof(esp_bd_addr_t)) != 0) {
            ESP_LOGW(TAG, "Rejecting non-transmitter peer");
            esp_ble_gap_disconnect(param->connect.remote_bda);
            break;
        }
        s_conn_id = param->connect.conn_id;
        s_echo_notify_enabled = false;
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "DISCONNECT reason=0x%02x", param->disconnect.reason);
        s_conn_id = 0xFFFF;
        s_echo_notify_enabled = false;
        esp_ble_gap_start_advertising(&s_adv_params);
        if (s_strip) {
            led_strip_clear(s_strip);
            led_strip_refresh(s_strip);
        }
        break;

    default:
        break;
    }
}

void app_main(void)
{
    led_init_rgb();

    const esp_timer_create_args_t targs = {
        .callback = &led_off_timer_cb,
        .name = "led_off",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_led_off_timer));

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
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(TOF_APP_ID));

    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));

    ESP_LOGI(TAG, "Receiver firmware — expect TX MAC " ESP_BD_ADDR_STR,
             ESP_BD_ADDR_HEX(k_transmitter_bda));
}

#elif CONFIG_TOF_ROLE_TRANSMITTER

// -------------------- Transmitter: GATT client state --------------------
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0xFFFF;
static bool s_connected;
static bool s_got_svc;
static bool s_write_in_flight;

static uint16_t s_rx_start;
static uint16_t s_rx_end;
static uint16_t s_echo_start;
static uint16_t s_echo_end;
static bool s_have_rx_range;
static bool s_have_echo_range;
static uint16_t s_rx_char_hdl;
static uint16_t s_echo_char_hdl;
static uint16_t s_echo_cccd_hdl;

static uint8_t s_wr_buf[TOF_PAYLOAD_LEN];
static int64_t s_t_write_us;

static void start_write_task(void);

static void gap_event_handler_tx(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
        esp_ble_gap_cb_param_t *sr = param;
        if (sr->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
            return;
        }
        if (memcmp(sr->scan_rst.bda, k_receiver_bda, sizeof(esp_bd_addr_t)) != 0) {
            return;
        }
        if (s_connected) {
            return;
        }

        ESP_LOGI(TAG, "Receiver seen, connecting…");
        esp_ble_gap_stop_scanning();
        if (s_gattc_if != ESP_GATT_IF_NONE) {
            esp_ble_gattc_open(s_gattc_if, sr->scan_rst.bda,
                               (esp_ble_addr_type_t)sr->scan_rst.ble_addr_type, true);
        }
        return;
    }

    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_scanning(0);
        }
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "scan start failed");
        } else {
            ESP_LOGI(TAG, "Scanning for receiver " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(k_receiver_bda));
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        break;
    default:
        break;
    }
}

static bool uuid128_eq(const uint8_t *a, esp_bt_uuid_t *u)
{
    if (u->len != ESP_UUID_LEN_128) {
        return false;
    }
    return memcmp(a, u->uuid.uuid128, 16) == 0;
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATTC REG_EVT");
        s_gattc_if = gattc_if;
        esp_ble_scan_params_t sp = {0};
        sp.scan_type = BLE_SCAN_TYPE_ACTIVE;
        sp.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        sp.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
        sp.scan_interval = 0x50;
        sp.scan_window = 0x30;
        sp.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
        esp_ble_gap_set_scan_params(&sp);
        break;

    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "GATTC CONNECT conn_id=%u remote " ESP_BD_ADDR_STR,
                 param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        s_got_svc = false;
        s_have_rx_range = false;
        s_have_echo_range = false;
        esp_ble_gap_stop_scanning();
        esp_ble_gattc_search_service(gattc_if, param->connect.conn_id, NULL);
        break;

    case ESP_GATTC_OPEN_EVT:
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        esp_bt_uuid_t *svc_uuid = &param->search_res.srvc_id.uuid;
        if (svc_uuid->len == ESP_UUID_LEN_128 &&
            uuid128_eq(tof_rx_svc_uuid128, svc_uuid)) {
            s_rx_start = param->search_res.start_handle;
            s_rx_end = param->search_res.end_handle;
            s_have_rx_range = true;
            ESP_LOGI(TAG, "Found RX svc hdl [%u,%u]", s_rx_start, s_rx_end);
        } else if (svc_uuid->len == ESP_UUID_LEN_128 &&
                   uuid128_eq(tof_echo_svc_uuid128, svc_uuid)) {
            s_echo_start = param->search_res.start_handle;
            s_echo_end = param->search_res.end_handle;
            s_have_echo_range = true;
            ESP_LOGI(TAG, "Found Echo svc hdl [%u,%u]", s_echo_start, s_echo_end);
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "service search failed");
            break;
        }
        if (!s_have_rx_range) {
            ESP_LOGE(TAG, "RX service not found");
            break;
        }

        esp_bt_uuid_t rx_chr = {0};
        rx_chr.len = ESP_UUID_LEN_128;
        memcpy(rx_chr.uuid.uuid128, tof_rx_chr_uuid128, 16);
        esp_gattc_char_elem_t rx_el;
        uint16_t cnt = 1;
        if (esp_ble_gattc_get_char_by_uuid(gattc_if, s_conn_id, s_rx_start, s_rx_end, rx_chr,
                                           &rx_el, &cnt) != ESP_GATT_OK ||
            cnt == 0) {
            ESP_LOGE(TAG, "RX char not found");
            break;
        }
        s_rx_char_hdl = rx_el.char_handle;

        if (s_have_echo_range) {
            esp_bt_uuid_t echo_chr = {0};
            echo_chr.len = ESP_UUID_LEN_128;
            memcpy(echo_chr.uuid.uuid128, tof_echo_chr_uuid128, 16);
            esp_gattc_char_elem_t ee;
            cnt = 1;
            if (esp_ble_gattc_get_char_by_uuid(gattc_if, s_conn_id, s_echo_start, s_echo_end,
                                               echo_chr, &ee, &cnt) == ESP_GATT_OK && cnt > 0) {
                s_echo_char_hdl = ee.char_handle;

                esp_bt_uuid_t cccd_u = {0};
                cccd_u.len = ESP_UUID_LEN_16;
                cccd_u.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
                esp_gattc_descr_elem_t de;
                cnt = 1;
                if (esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id, s_echo_char_hdl,
                                                           cccd_u, &de, &cnt) == ESP_GATT_OK &&
                    cnt > 0) {
                    s_echo_cccd_hdl = de.handle;
                    uint16_t en = 1;
                    esp_ble_gattc_write_char_descr(gattc_if, s_conn_id, s_echo_cccd_hdl,
                                                   sizeof(en), (uint8_t *)&en,
                                                   ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                }
            }
        }

        ESP_LOGI(TAG, "Discovery done, RX char_hdl=%u", s_rx_char_hdl);
        s_got_svc = true;
        start_write_task();
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        break;

    case ESP_GATTC_NOTIFY_EVT:
        if (param->notify.handle == s_echo_char_hdl && param->notify.value_len >= TOF_ECHO_PAYLOAD_LEN) {
            uint64_t t_here = (uint64_t)esp_timer_get_time();
            uint64_t t_rx = 0;
            memcpy(&t_rx, param->notify.value + TOF_PAYLOAD_LEN, sizeof(t_rx));
            ESP_LOGI(TAG, "Echo: t_rx_on_peer=%" PRIu64 " us, notify_seen_here=%" PRIu64 " us",
                     t_rx, t_here);
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.handle == s_rx_char_hdl && s_write_in_flight) {
            if (param->write.status == ESP_GATT_OK) {
                int64_t t1 = esp_timer_get_time();
                uint64_t rtt = (uint64_t)(t1 - s_t_write_us);
                uint32_t seq = (uint32_t)s_wr_buf[0] | ((uint32_t)s_wr_buf[1] << 8) |
                               ((uint32_t)s_wr_buf[2] << 16) | ((uint32_t)s_wr_buf[3] << 24);
                ESP_LOGI(TAG, "seq=%" PRIu32 " write-RSP RTT=%" PRIu64
                             " us (~one-way proxy %" PRIu64 " us if symmetric)",
                         seq, rtt, rtt / 2);
            } else {
                ESP_LOGW(TAG, "write-RSP failed, status=%d", param->write.status);
            }
            s_write_in_flight = false;
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "GATTC DISCONNECT");
        s_connected = false;
        s_conn_id = 0xFFFF;
        s_got_svc = false;
        s_have_rx_range = false;
        s_have_echo_range = false;
        s_rx_start = s_rx_end = s_echo_start = s_echo_end = 0;
        s_write_in_flight = false;
        esp_ble_gap_start_scanning(0);
        break;

    default:
        break;
    }
}

static void write_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t seq = 0;

    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TOF_WRITE_PERIOD_MS));

        if (!s_connected || !s_got_svc || s_gattc_if == ESP_GATT_IF_NONE || s_write_in_flight) {
            continue;
        }

        uint64_t t_us = (uint64_t)esp_timer_get_time();
        s_wr_buf[0] = (uint8_t)(seq);
        s_wr_buf[1] = (uint8_t)(seq >> 8);
        s_wr_buf[2] = (uint8_t)(seq >> 16);
        s_wr_buf[3] = (uint8_t)(seq >> 24);
        memcpy(s_wr_buf + 4, &t_us, sizeof(t_us));

        s_t_write_us = esp_timer_get_time();
        s_write_in_flight = true;

        esp_err_t w = esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_rx_char_hdl,
                                               TOF_PAYLOAD_LEN, s_wr_buf,
                                               ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        if (w != ESP_OK) {
            ESP_LOGW(TAG, "write_char failed: %s", esp_err_to_name(w));
            s_write_in_flight = false;
        }
        seq++;
    }
}

static TaskHandle_t s_write_th;

static void start_write_task(void)
{
    if (s_write_th != NULL) {
        return;
    }
    xTaskCreate(write_task, "tof_write", 4096, NULL, 5, &s_write_th);
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

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler_tx));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(TOF_APP_ID));

    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));

    ESP_LOGI(TAG, "Transmitter firmware — connects to " ESP_BD_ADDR_STR,
             ESP_BD_ADDR_HEX(k_receiver_bda));
}

#else
#error "Select TOF_ROLE_RECEIVER or TOF_ROLE_TRANSMITTER in menuconfig."
#endif
