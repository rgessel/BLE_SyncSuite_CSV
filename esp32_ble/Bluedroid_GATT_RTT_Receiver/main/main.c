/*
 * BLE GATT peripheral that acts as an RTT "reflector":
 * - Advertises as "ESP_RTT_RX"
 * - Accepts writes on a custom characteristic; echoes payload and replies with NOTIFY.
 * - WS2812 on GPIO2: green pulse on each ping write (LED_PULSE_MS), non-blocking via esp_timer.
 *
 * RF time-of-flight is negligible compared to the BLE connection interval and host
 * stack latency. Pairs with Bluedroid_GATT_RTT_Client for measured round-trip time.
 */

#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#include "led_strip.h"

#define RTT_DEVICE_NAME "ESP_RTT_RX"

#define PING_WRITE_LEN   8
#define PING_NOTIFY_LEN  16

#define LED_GPIO      2
#define LED_COUNT     1
#define LED_PULSE_MS  250

/* Must match Bluedroid_GATT_RTT_Client */
static const uint8_t rtt_svc_uuid128[ESP_UUID_LEN_128] = {
    0x24, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0xA1, 0xA1, 0x16, 0x00,
};

static const uint8_t rtt_chr_uuid128[ESP_UUID_LEN_128] = {
    0x25, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0xA1, 0xA1, 0x16, 0x00,
};

#define RTT_NUM_HANDLE 8

#define ADV_CONFIG_FLAG      (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)

static const char *TAG = "RTT_RX";

static led_strip_handle_t s_strip = NULL;
static esp_timer_handle_t s_led_off_timer = NULL;

static uint8_t adv_config_done = 0;

static esp_gatt_if_t g_gatts_if = ESP_GATT_IF_NONE;
static uint16_t g_service_handle = 0;
static uint16_t g_char_handle = 0;
static uint16_t g_cccd_handle = 0;

static bool notify_enabled = false;

static uint8_t ping_value[PING_NOTIFY_LEN];

static esp_attr_value_t ping_attr = {
    .attr_max_len = PING_NOTIFY_LEN,
    .attr_len     = PING_NOTIFY_LEN,
    .attr_value   = ping_value,
};

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = ESP_UUID_LEN_128,
    .p_service_uuid = (uint8_t *)rtt_svc_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x40,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void led_init_rgb(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
    };

#if CONFIG_RTT_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
#elif CONFIG_RTT_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_cfg = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_cfg, &spi_cfg, &s_strip));
#else
#error "LED strip backend not set (menuconfig: RTT Receiver LED)"
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

static void led_off_timer_cb(void *arg)
{
    (void)arg;
    led_set_green(false);
}

static void led_pulse_on_ping(void)
{
    led_set_green(true);
    if (s_led_off_timer) {
        esp_timer_stop(s_led_off_timer);
        esp_timer_start_once(s_led_off_timer, (uint64_t)LED_PULSE_MS * 1000ULL);
    }
}

static void write_rsp_if_needed(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (param->write.need_rsp) {
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= ~ADV_CONFIG_FLAG;
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= ~SCAN_RSP_CONFIG_FLAG;
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed, status=%d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising started");
        }
        break;

    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTS_REG_EVT: {
        ESP_LOGI(TAG, "REG_EVT status=%d", param->reg.status);
        g_gatts_if = gatts_if;

        esp_err_t err = esp_ble_gap_set_device_name(RTT_DEVICE_NAME);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_device_name failed: %s", esp_err_to_name(err));
        }

        adv_config_done = ADV_CONFIG_FLAG;
        err = esp_ble_gap_config_adv_data(&adv_data);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "config_adv_data failed: %s", esp_err_to_name(err));
            break;
        }

        esp_gatt_srvc_id_t service_id = {0};
        service_id.is_primary = true;
        service_id.id.inst_id = 0;
        service_id.id.uuid.len = ESP_UUID_LEN_128;
        memcpy(service_id.id.uuid.uuid.uuid128, rtt_svc_uuid128, ESP_UUID_LEN_128);

        esp_ble_gatts_create_service(gatts_if, &service_id, RTT_NUM_HANDLE);
        break;
    }

    case ESP_GATTS_CREATE_EVT: {
        g_service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(g_service_handle);

        esp_bt_uuid_t char_uuid = {0};
        char_uuid.len = ESP_UUID_LEN_128;
        memcpy(char_uuid.uuid.uuid128, rtt_chr_uuid128, ESP_UUID_LEN_128);

        esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_READ
            | ESP_GATT_CHAR_PROP_BIT_WRITE
            | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

        esp_err_t ret = esp_ble_gatts_add_char(
            g_service_handle,
            &char_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            prop,
            &ping_attr,
            NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "add_char failed: %s", esp_err_to_name(ret));
        }
        break;
    }

    case ESP_GATTS_ADD_CHAR_EVT: {
        g_char_handle = param->add_char.attr_handle;

        esp_bt_uuid_t cccd_uuid = {0};
        cccd_uuid.len = ESP_UUID_LEN_16;
        cccd_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

        esp_err_t ret = esp_ble_gatts_add_char_descr(
            g_service_handle,
            &cccd_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            NULL,
            NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "add_char_descr failed: %s", esp_err_to_name(ret));
        }
        break;
    }

    case ESP_GATTS_ADD_CHAR_DESCR_EVT: {
        g_cccd_handle = param->add_char_descr.attr_handle;
        ESP_LOGI(TAG, "GATT ready (char=%u cccd=%u)", g_char_handle, g_cccd_handle);
        break;
    }

    case ESP_GATTS_READ_EVT: {
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = PING_NOTIFY_LEN;
        memcpy(rsp.attr_value.value, ping_value, PING_NOTIFY_LEN);
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
        break;
    }

    case ESP_GATTS_WRITE_EVT: {
        if (param->write.handle == g_cccd_handle && param->write.len == 2) {
            uint16_t cccd = (uint16_t)(param->write.value[0] | (param->write.value[1] << 8));
            notify_enabled = (cccd & 0x0001) != 0;
            ESP_LOGI(TAG, "CCCD -> notify %s", notify_enabled ? "ON" : "OFF");
        } else if (param->write.handle == g_char_handle && param->write.len == PING_WRITE_LEN) {
            led_pulse_on_ping();

            uint64_t t_rx = (uint64_t)esp_timer_get_time();

            memcpy(ping_value, param->write.value, PING_WRITE_LEN);
            for (int i = 0; i < 8; i++) {
                ping_value[PING_WRITE_LEN + i] = (uint8_t)(t_rx >> (8 * i));
            }

            (void)esp_ble_gatts_set_attr_value(g_char_handle, PING_NOTIFY_LEN, ping_value);
            write_rsp_if_needed(gatts_if, param);

            if (notify_enabled) {
                esp_err_t err = esp_ble_gatts_send_indicate(
                    gatts_if,
                    param->write.conn_id,
                    g_char_handle,
                    PING_NOTIFY_LEN,
                    ping_value,
                    false);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "send_indicate failed: %s", esp_err_to_name(err));
                }
            }
            break;
        }

        write_rsp_if_needed(gatts_if, param);
        break;
    }

    case ESP_GATTS_CONNECT_EVT: {
        ESP_LOGI(TAG, "CONNECTED conn_id=%u " ESP_BD_ADDR_STR,
                 param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        notify_enabled = false;
        break;
    }

    case ESP_GATTS_DISCONNECT_EVT: {
        ESP_LOGI(TAG, "DISCONNECTED reason=0x%02x", param->disconnect.reason);
        notify_enabled = false;
        if (s_led_off_timer) {
            esp_timer_stop(s_led_off_timer);
        }
        led_set_green(false);
        esp_ble_gap_start_advertising(&adv_params);
        break;
    }

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
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));

    ret = esp_ble_gatt_set_local_mtu(247);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set_local_mtu: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "RTT receiver running. Connect from ESP_RTT client.");
}
