#include "BLE_Server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// NimBLE headers
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE";

// --- Fallback chunk size (BLE 4.0 minimum payload) ---
// Used only if MTU has not yet been negotiated.
#define BLE_CHUNK_SIZE_MIN  20

// --- End-of-message sentinel ---
// Sent as a 1-byte notification after the last data chunk so the client
// can detect end-of-message without relying on short chunk length.
#define BLE_EOT_BYTE        0x04

// --- 128-bit UUIDs ---
// Service UUID: 12345678-1234-1234-1234-123456789ABC
static const ble_uuid128_t SERVICE_UUID =
    BLE_UUID128_INIT(0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                     0x78, 0x56, 0x34, 0x12);

// Characteristic UUID: 12345678-1234-1234-1234-123456789ABD
static const ble_uuid128_t CHAR_UUID =
    BLE_UUID128_INIT(0xBD, 0x9A, 0x78, 0x56, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                     0x78, 0x56, 0x34, 0x12);

// --- Internal state ---
static bool     g_connected       = false;
static bool     g_subscribed      = false;  // client has enabled notifications
static uint16_t g_conn_handle     = 0;
static uint16_t g_char_val_handle = 0;      // handle assigned by NimBLE at registration
static uint16_t g_mtu             = BLE_CHUNK_SIZE_MIN + 3; // negotiated ATT MTU (default 23)

// --- Forward declarations ---
static void ble_advertise(void);

// -------------------------------------------------------
// GATT characteristic access callback
// -------------------------------------------------------

/**
 * @brief Called by NimBLE when a client reads or writes the characteristic.
 *        We only use notify so reads return empty and writes are ignored.
 */
static int gatt_char_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // Nothing to handle — characteristic is notify only
    return 0;
}

// -------------------------------------------------------
// GATT service definition
// -------------------------------------------------------

static const struct ble_gatt_svc_def g_gatt_services[] = 
{
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) 
        {
            {
                // Notify characteristic — ESP32 pushes data to client
                .uuid       = &CHAR_UUID.u,
                .access_cb  = gatt_char_access_cb,
                .val_handle = &g_char_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                0,  // terminator
            }
        },
    },
    {
        0,  // terminator
    }
};

// -------------------------------------------------------
// GAP event handler — connection, disconnection, MTU, subscribe
// -------------------------------------------------------

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0)
            {
                // Client connected — wait for subscription before sending
                g_connected   = true;
                g_subscribed  = false;
                g_conn_handle = event->connect.conn_handle;
                // Reset MTU to default until negotiated
                g_mtu = BLE_CHUNK_SIZE_MIN + 3;
                ESP_LOGI(TAG, "Client connected — handle %d", g_conn_handle);
            }
            else
            {
                // Connection failed — restart advertising
                ESP_LOGW(TAG, "Connection failed, restarting advertising");
                g_connected  = false;
                g_subscribed = false;
                ble_advertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            // Client disconnected — clear all session state and re-advertise
            ESP_LOGI(TAG, "Client disconnected — reason %d", event->disconnect.reason);
            g_connected  = false;
            g_subscribed = false;
            ble_advertise();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            // Advertising window ended — restart
            ESP_LOGI(TAG, "Advertising complete, restarting");
            ble_advertise();
            break;

        case BLE_GAP_EVENT_MTU:
            // Client and server have negotiated a new ATT MTU
            g_mtu = event->mtu.value;
            ESP_LOGI(TAG, "MTU negotiated: %d bytes (payload: %d bytes)", g_mtu, g_mtu - 3);
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            // Client wrote to the CCCD — track whether notifications are enabled
            g_subscribed = (bool)event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Client %s notifications", g_subscribed ? "subscribed to" : "unsubscribed from");
            break;

        default:
            break;
    }

    return 0;
}

// -------------------------------------------------------
// Advertising
// -------------------------------------------------------

static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields      = {0};

    // Set device name in advertising packet
    fields.name             = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len         = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    // Set flags — general discoverable, BLE only
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to set advertising fields: %d", rc);
        return;
    }

    // Undirected connectable advertising
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to start advertising: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising as \"%s\"", BLE_DEVICE_NAME);
}

// -------------------------------------------------------
// NimBLE host sync callback — called when stack is ready
// -------------------------------------------------------

static void ble_on_sync(void)
{
    // Confirm we have a valid address
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to ensure address: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE stack synced — starting advertising");
    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE stack reset — reason %d", reason);
    g_connected  = false;
    g_subscribed = false;
}

// -------------------------------------------------------
// NimBLE host task
// -------------------------------------------------------

static void nimble_host_task(void *param)
{
    // This runs the NimBLE host indefinitely
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// -------------------------------------------------------
// Public API
// -------------------------------------------------------

int BLE_init(void)
{
    // NimBLE requires NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize NimBLE port
    int rc = nimble_port_init();
    if (rc != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return rc;
    }

    // Register sync and reset callbacks
    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    // Initialize GATT and GAP services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Register our custom GATT service
    rc = ble_gatts_count_cfg(g_gatt_services);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return rc;
    }

    rc = ble_gatts_add_svcs(g_gatt_services);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return rc;
    }

    // Set device name
    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to set device name: %d", rc);
        return rc;
    }

    // Start NimBLE host task
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE init complete");
    return 0;
}

bool BLE_is_connected(void)
{
    // Only report ready if the client has also subscribed to notifications
    return g_connected && g_subscribed;
}

int BLE_send_payload(const char *json, size_t json_len)
{
    if (!g_connected)
    {
        ESP_LOGW(TAG, "BLE_send_payload: no client connected");
        return -1;
    }

    if (!g_subscribed)
    {
        ESP_LOGW(TAG, "BLE_send_payload: client connected but not subscribed");
        return -1;
    }

    // Derive chunk size from negotiated MTU (ATT overhead is 3 bytes)
    // Falls back to 20 if MTU is somehow below minimum
    uint16_t payload_mtu = (g_mtu > 3) ? (g_mtu - 3) : BLE_CHUNK_SIZE_MIN;

    size_t offset = 0;

    while (offset < json_len)
    {
        size_t remaining  = json_len - offset;
        size_t chunk_len  = (remaining > payload_mtu) ? payload_mtu : remaining;

        // Attempt to send with retries to handle transient mbuf exhaustion
        int rc      = -1;
        int retries = 3;

        while (retries--)
        {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(json + offset, chunk_len);
            if (!om)
            {
                ESP_LOGW(TAG, "mbuf alloc failed at offset %d, retrying...", (int)offset);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            rc = ble_gatts_notify_custom(g_conn_handle, g_char_val_handle, om);
            if (rc == 0)
            {
                break;
            }

            ESP_LOGW(TAG, "Notify failed at offset %d (rc=%d), retrying...", (int)offset, rc);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (rc != 0)
        {
            ESP_LOGE(TAG, "Notify failed after retries at offset %d: %d", (int)offset, rc);
            return rc;
        }

        offset += chunk_len;

        // Small delay between chunks so client can process each one
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Send EOT sentinel so client reliably knows the message is complete,
    // regardless of whether the last chunk was full-sized.
    struct os_mbuf *eot = ble_hs_mbuf_from_flat(&(uint8_t){BLE_EOT_BYTE}, 1);
    if (eot)
    {
        ble_gatts_notify_custom(g_conn_handle, g_char_val_handle, eot);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to allocate EOT mbuf");
    }

    int chunks = (int)((json_len + payload_mtu - 1) / payload_mtu);
    ESP_LOGI(TAG, "Sent %d bytes in %d chunks (MTU payload: %d)", (int)json_len, chunks, payload_mtu);

    return 0;
}