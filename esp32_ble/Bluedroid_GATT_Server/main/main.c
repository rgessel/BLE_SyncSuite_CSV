/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Minimal ESP-IDF BLE GATT server:
 * - Advertises
 * - Exposes 1 service (0x181A) with 1 NOTIFY characteristic (128-bit UUID) + CCCD
 * - Sends notifications at a fixed rate: once per second (SENSOR_PERIOD_MS = 1000)
 *   payload (12 bytes, little-endian):
 *     [0..3]  = seq (uint32)
 *     [4..11] = t_us (uint64) microseconds since boot
 * - RGB LED (WS2812 via led_strip) "pulses" GREEN on each successful send:
 *     ON for LED_PULSE_MS (250 ms), then OFF.
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
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gatt_common_api.h"

#include "led_strip.h"

// -------------------- Tunables --------------------
#define SENSOR_PERIOD_MS   250  // four times per second
#define LED_PULSE_MS       250   // LED on for 250ms after send
#define SENSOR_PAYLOAD_LEN 12    // seq(4) + t_us(8)

// -------------------- RGB LED (WS2812) --------------------
#define LED_GPIO   2
#define LED_COUNT  1

static led_strip_handle_t strip = NULL;

// -------------------- UUIDs --------------------
#define SENSOR_SVC_UUID 0x181A  // Environmental Sensing (convenient 16-bit service UUID)

// Custom 128-bit characteristic UUID (keep consistent with Android side!)
static const uint8_t sensor_chr_uuid128[16] = {
    // 0015a1a1-1212-efde-1523-785feabcd123 (example)
    0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0xA1, 0xA1, 0x15, 0x00
};

#define SENSOR_NUM_HANDLE 6

// Advertising config flags
#define ADV_CONFIG_FLAG      (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)

static const char *TAG = "BLE_SENSOR_RGB";

// -------------------- BLE/GATT state --------------------
static uint8_t adv_config_done = 0;

static bool sensor_ready = false;
static bool notify_enabled = false;

static esp_gatt_if_t g_gatts_if = ESP_GATT_IF_NONE;
static uint16_t g_conn_id = 0xFFFF;

static uint16_t g_service_handle = 0;
static uint16_t g_char_handle = 0;
static uint16_t g_cccd_handle = 0;

static uint8_t sensor_value[SENSOR_PAYLOAD_LEN] = {0};

static esp_attr_value_t sensor_attr = {
    .attr_max_len = SENSOR_PAYLOAD_LEN,
    .attr_len     = SENSOR_PAYLOAD_LEN,
    .attr_value   = sensor_value,
};

// -------------------- Advertising --------------------
static esp_ble_adv_data_t adv_data = {
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
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,  // 20ms
    .adv_int_max        = 0x40,  // 40ms
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// -------------------- LED Helpers --------------------
static void led_init_rgb(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
    };

#if CONFIG_EXAMPLE_BLINK_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip));
#elif CONFIG_EXAMPLE_BLINK_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_cfg = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_cfg, &spi_cfg, &strip));
#else
#error "LED strip backend not set. Enable RMT or SPI backend in menuconfig."
#endif

    led_strip_clear(strip);
    led_strip_refresh(strip);
}

static void led_set_green(bool on)
{
    if (!strip) return;

    if (on) {
        led_strip_set_pixel(strip, 0, 0, 255, 0); // GREEN
        led_strip_refresh(strip);
    } else {
        led_strip_clear(strip);
        led_strip_refresh(strip);
    }
}

static void write_rsp_if_needed(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (param->write.need_rsp) {
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
    }
}

// -------------------- Periodic notify task --------------------
static void sensor_notify_task(void *param)
{
    ESP_LOGI(TAG, "Notify task start. Period=%d ms, LED pulse=%d ms", SENSOR_PERIOD_MS, LED_PULSE_MS);

    uint32_t seq = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSOR_PERIOD_MS));

        if (!sensor_ready || !notify_enabled || g_gatts_if == ESP_GATT_IF_NONE) {
            continue;
        }

        // Build payload: [seq:u32 LE][t_us:u64 LE]
        uint64_t t_us = (uint64_t)esp_timer_get_time();

        sensor_value[0] = (uint8_t)(seq);
        sensor_value[1] = (uint8_t)(seq >> 8);
        sensor_value[2] = (uint8_t)(seq >> 16);
        sensor_value[3] = (uint8_t)(seq >> 24);

        sensor_value[4]  = (uint8_t)(t_us);
        sensor_value[5]  = (uint8_t)(t_us >> 8);
        sensor_value[6]  = (uint8_t)(t_us >> 16);
        sensor_value[7]  = (uint8_t)(t_us >> 24);
        sensor_value[8]  = (uint8_t)(t_us >> 32);
        sensor_value[9]  = (uint8_t)(t_us >> 40);
        sensor_value[10] = (uint8_t)(t_us >> 48);
        sensor_value[11] = (uint8_t)(t_us >> 56);

        // Keep attribute value consistent for reads
        (void)esp_ble_gatts_set_attr_value(g_char_handle, SENSOR_PAYLOAD_LEN, sensor_value);

        // Send NOTIFY (confirm=false)
        esp_err_t err = esp_ble_gatts_send_indicate(
            g_gatts_if,
            g_conn_id,
            g_char_handle,
            SENSOR_PAYLOAD_LEN,
            sensor_value,
            false
        );

        if (err == ESP_OK) {
            // Pulse LED ON for LED_PULSE_MS, then OFF
            led_set_green(true);
            vTaskDelay(pdMS_TO_TICKS(LED_PULSE_MS));
            led_set_green(false);
        } else {
            ESP_LOGW(TAG, "send notify failed: %s", esp_err_to_name(err));
        }

        seq++;
    }
}

// -------------------- GAP callback --------------------
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Adv data set complete, status=%d", param->adv_data_cmpl.status);
        adv_config_done &= (~ADV_CONFIG_FLAG);
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan rsp data set complete, status=%d", param->scan_rsp_data_cmpl.status);
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Adv start failed, status=%d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising started");
        }
        break;

    default:
        break;
    }
}

// -------------------- GATTS callback --------------------
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTS_REG_EVT: {
        ESP_LOGI(TAG, "REG_EVT status=%d app_id=%d", param->reg.status, param->reg.app_id);
        g_gatts_if = gatts_if;

        // Configure advertising
        adv_config_done = ADV_CONFIG_FLAG;
        esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret) {
            ESP_LOGE(TAG, "config adv data failed: %s", esp_err_to_name(ret));
            break;
        }

        // Create service (0x181A)
        esp_gatt_srvc_id_t service_id = {0};
        service_id.is_primary = true;
        service_id.id.inst_id = 0x00;
        service_id.id.uuid.len = ESP_UUID_LEN_16;
        service_id.id.uuid.uuid.uuid16 = SENSOR_SVC_UUID;

        esp_ble_gatts_create_service(gatts_if, &service_id, SENSOR_NUM_HANDLE);
        break;
    }

    case ESP_GATTS_CREATE_EVT: {
        ESP_LOGI(TAG, "CREATE_EVT status=%d service_handle=%d", param->create.status, param->create.service_handle);
        g_service_handle = param->create.service_handle;

        esp_ble_gatts_start_service(g_service_handle);

        // Add notify characteristic (128-bit)
        esp_bt_uuid_t char_uuid = {0};
        char_uuid.len = ESP_UUID_LEN_128;
        memcpy(char_uuid.uuid.uuid128, sensor_chr_uuid128, ESP_UUID_LEN_128);

        esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

        esp_err_t ret = esp_ble_gatts_add_char(
            g_service_handle,
            &char_uuid,
            ESP_GATT_PERM_READ,
            prop,
            &sensor_attr,
            NULL
        );
        if (ret) {
            ESP_LOGE(TAG, "add char failed: %s", esp_err_to_name(ret));
        }
        break;
    }

    case ESP_GATTS_ADD_CHAR_EVT: {
        ESP_LOGI(TAG, "ADD_CHAR_EVT status=%d attr_handle=%d", param->add_char.status, param->add_char.attr_handle);
        g_char_handle = param->add_char.attr_handle;

        // Add CCCD (0x2902)
        esp_bt_uuid_t cccd_uuid = {0};
        cccd_uuid.len = ESP_UUID_LEN_16;
        cccd_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

        esp_err_t ret = esp_ble_gatts_add_char_descr(
            g_service_handle,
            &cccd_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            NULL,
            NULL
        );
        if (ret) {
            ESP_LOGE(TAG, "add cccd failed: %s", esp_err_to_name(ret));
        }
        break;
    }

    case ESP_GATTS_ADD_CHAR_DESCR_EVT: {
        ESP_LOGI(TAG, "ADD_DESCR_EVT status=%d descr_handle=%d",
                 param->add_char_descr.status, param->add_char_descr.attr_handle);
        g_cccd_handle = param->add_char_descr.attr_handle;
        sensor_ready = true;
        break;
    }

    case ESP_GATTS_READ_EVT: {
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = SENSOR_PAYLOAD_LEN;
        memcpy(rsp.attr_value.value, sensor_value, SENSOR_PAYLOAD_LEN);

        esp_ble_gatts_send_response(gatts_if,
                                    param->read.conn_id,
                                    param->read.trans_id,
                                    ESP_GATT_OK,
                                    &rsp);
        break;
    }

    case ESP_GATTS_WRITE_EVT: {
        // CCCD write enables/disables notifications
        if (param->write.handle == g_cccd_handle && param->write.len == 2) {
            uint16_t cccd = (uint16_t)((param->write.value[1] << 8) | param->write.value[0]);

            if (cccd == 0x0001) {
                notify_enabled = true;
                ESP_LOGI(TAG, "Notifications ENABLED");
            } else if (cccd == 0x0000) {
                notify_enabled = false;
                ESP_LOGI(TAG, "Notifications DISABLED");
            } else {
                ESP_LOGW(TAG, "Unknown CCCD value: 0x%04x", cccd);
            }
        }

        write_rsp_if_needed(gatts_if, param);
        break;
    }

    case ESP_GATTS_CONNECT_EVT: {
        ESP_LOGI(TAG, "CONNECT conn_id=%u remote " ESP_BD_ADDR_STR,
                 param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        g_conn_id = param->connect.conn_id;
        notify_enabled = false; // require CCCD write after connect
        break;
    }

    case ESP_GATTS_DISCONNECT_EVT: {
        ESP_LOGI(TAG, "DISCONNECT remote " ESP_BD_ADDR_STR " reason=0x%02x",
                 ESP_BD_ADDR_HEX(param->disconnect.remote_bda), param->disconnect.reason);
        notify_enabled = false;
        esp_ble_gap_start_advertising(&adv_params);

        // Turn LED off on disconnect
        if (strip) {
            led_strip_clear(strip);
            led_strip_refresh(strip);
        }
        break;
    }

    default:
        break;
    }
}

// -------------------- app_main --------------------
void app_main(void)
{
    // RGB LED init (no mic logic)
    led_init_rgb();

    // NVS is required for BLE
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // BLE only
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Init controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "bt controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "bt controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    // Init + enable bluedroid
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    // Register callbacks
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gap register error: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gatts register error: %s", esp_err_to_name(ret));
        return;
    }

    // Register one GATT app (id 0)
    ret = esp_ble_gatts_app_register(0);
    if (ret) {
        ESP_LOGE(TAG, "app register error: %s", esp_err_to_name(ret));
        return;
    }

    // Optional MTU
    ret = esp_ble_gatt_set_local_mtu(500);
    if (ret) {
        ESP_LOGW(TAG, "set local MTU failed: %s", esp_err_to_name(ret));
    }

    // Start periodic notify task
    xTaskCreate(sensor_notify_task, "sensor_notify", 3 * 1024, NULL, 5, NULL);
}
