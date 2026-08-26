#include "ble_control.h"

#include <stdio.h>
#include <string.h>

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "led_controller.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"

#define COMMAND_MAX_BYTES 255
#define RESPONSE_MAX_BYTES 128

static const char *TAG = "ble";
static uint8_t s_address_type;
static uint16_t s_value_handle;
static char s_last_response[RESPONSE_MAX_BYTES] =
    "{\"ok\":true,\"mode\":\"off\"}";

/* 7b9a0001-6d4f-4f4b-9f2a-1c5e7a3d1000 */
static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x00, 0x10, 0x3d, 0x7a, 0x5e, 0x1c, 0x2a, 0x9f,
    0x4b, 0x4f, 0x4f, 0x6d, 0x01, 0x00, 0x9a, 0x7b);

/* 7b9a0002-6d4f-4f4b-9f2a-1c5e7a3d1000 */
static const ble_uuid128_t s_characteristic_uuid = BLE_UUID128_INIT(
    0x00, 0x10, 0x3d, 0x7a, 0x5e, 0x1c, 0x2a, 0x9f,
    0x4b, 0x4f, 0x4f, 0x6d, 0x02, 0x00, 0x9a, 0x7b);

static int characteristic_access(uint16_t connection_handle,
                                 uint16_t attribute_handle,
                                 struct ble_gatt_access_ctxt *context,
                                 void *arg)
{
    (void)connection_handle;
    (void)attribute_handle;
    (void)arg;

    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(context->om, s_last_response,
                              strlen(s_last_response)) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t length = OS_MBUF_PKTLEN(context->om);
    if (length == 0 || length > COMMAND_MAX_BYTES) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char command[COMMAND_MAX_BYTES + 1];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(context->om, command, length, &copied) != 0 ||
        copied != length) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    command[length] = '\0';

    led_controller_handle_command(command, s_last_response,
                                  sizeof(s_last_response));
    ble_gatts_chr_updated(s_value_handle);
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_characteristic_uuid.u,
                .access_cb = characteristic_access,
                .val_handle = &s_value_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
        } else {
            ESP_LOGI(TAG, "client connected");
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "client disconnected; reason=%d", event->disconnect.reason);
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set advertising fields failed: %d", rc);
        return;
    }

    /* A 128-bit UUID and the full name do not fit together in the 31-byte
     * legacy advertising packet, so put the name in the scan response. */
    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields response_fields = {0};
    response_fields.name = (uint8_t *)name;
    response_fields.name_len = strlen(name);
    response_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set scan response failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_address_type, NULL, BLE_HS_FOREVER,
                           &parameters, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising failed: %d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_address_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address inference failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", CONFIG_BLE_DEVICE_NAME);
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_control_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        return rc;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_svc_gap_device_name_set(CONFIG_BLE_DEVICE_NAME);
    if (rc == 0) {
        rc = ble_gatts_count_cfg(s_gatt_services);
    }
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT initialization failed: %d", rc);
        return ESP_FAIL;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
