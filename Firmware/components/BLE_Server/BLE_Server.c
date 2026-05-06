/**
 * @file BLE_Server.c
 * @brief NimBLE GATT server implementation for the Sleep Monitor.
 *
 * Manages the full BLE lifecycle:
 *   - Initialises the NimBLE stack and registers a custom 128-bit GATT service
 *   - Advertises as "SleepMonitor" until a client connects
 *   - Tracks connection state, notification subscription, and negotiated MTU
 *   - Splits JSON payloads into MTU-sized chunks and sends each as a notification
 *   - Appends a single 0x04 (EOT) byte after the last chunk so the C# client
 *     can detect end-of-message without relying on chunk length
 *   - Re-advertises automatically on disconnect or failed connection
 */

#include "BLE_Server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE";

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/** @def BLE_CHUNK_SIZE_MIN
 *  @brief Fallback BLE payload size in bytes (BLE 4.0 minimum ATT payload).
 *
 *  Used only before MTU negotiation completes. Once the client negotiates
 *  a larger MTU, @ref g_mtu is updated and this value is no longer used.
 */
#define BLE_CHUNK_SIZE_MIN  20

/** @def BLE_EOT_BYTE
 *  @brief End-of-transmission sentinel byte.
 *
 *  Sent as a single 1-byte notification after the last JSON data chunk.
 *  Allows the C# client (PacketParser.cs) to detect end-of-message
 *  without relying on receiving a short final chunk.
 *  Value 0x04 follows the ASCII EOT control code convention.
 */
#define BLE_EOT_BYTE        0x04

// ─────────────────────────────────────────────────────────────────────────────
// UUIDs
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Primary GATT service UUID: 12345678-1234-1234-1234-123456789ABC
 *
 * NimBLE stores 128-bit UUIDs in little-endian byte order, so the bytes
 * in BLE_UUID128_INIT() appear in reverse compared to the canonical string.
 * Must match ServiceUuid in BleConstants.cs on the C# client.
 */
static const ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT
                    (0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                     0x78, 0x56, 0x34, 0x12);

/**
 * @brief Notify characteristic UUID: 12345678-1234-1234-1234-123456789ABD
 *
 * Identical to SERVICE_UUID except the last byte is 0xBD instead of 0xBC.
 * Must match CharacteristicUuid in BleConstants.cs on the C# client.
 */
static const ble_uuid128_t CHAR_UUID = BLE_UUID128_INIT
                    (0xBD, 0x9A, 0x78, 0x56, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                     0x78, 0x56, 0x34, 0x12);

// ─────────────────────────────────────────────────────────────────────────────
// Internal state
// ─────────────────────────────────────────────────────────────────────────────

/** @brief True when a client has an active GATT connection. */
static bool     g_connected       = false;

/** @brief True when the connected client has enabled notifications on the characteristic. */
static bool     g_subscribed      = false;

/** @brief NimBLE connection handle for the currently connected client. */
static uint16_t g_conn_handle     = 0;

/** @brief ATT attribute handle assigned by NimBLE when the characteristic is registered.
 *         Used as the target handle in ble_gatts_notify_custom(). */
static uint16_t g_char_val_handle = 0;

/** @brief Negotiated ATT MTU in bytes (default 23 = 20 byte payload + 3 byte ATT overhead).
 *         Updated in the BLE_GAP_EVENT_MTU handler when the client requests a larger MTU. */
static uint16_t g_mtu             = BLE_CHUNK_SIZE_MIN + 3;

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

static void ble_advertise(void);

// ─────────────────────────────────────────────────────────────────────────────
// GATT characteristic access callback
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief GATT characteristic read/write callback.
 *
 * Required by NimBLE for every registered characteristic, but the notify
 * characteristic is write-only from the server side — the client only
 * receives notifications, it never reads or writes this characteristic
 * directly. All incoming access attempts are silently ignored.
 *
 * @param conn_handle  Connection handle of the requesting client.
 * @param attr_handle  Attribute handle being accessed.
 * @param ctxt         GATT access context containing operation type and data.
 * @param arg          Optional user argument (unused).
 *
 * @return 0 always.
 */
static int gatt_char_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// GATT service definition
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Static GATT service table registered with NimBLE at init time.
 *
 * Defines one primary service containing one notify-only characteristic.
 * NimBLE populates @ref g_char_val_handle when the service is registered
 * via ble_gatts_add_svcs().
 */
static const struct ble_gatt_svc_def g_gatt_services[] =
{
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                /** Notify characteristic — server pushes JSON chunks to the client.
                 *  The client enables notifications by writing to the CCCD,
                 *  which triggers BLE_GAP_EVENT_SUBSCRIBE in gap_event_cb(). */
                .uuid       = &CHAR_UUID.u,
                .access_cb  = gatt_char_access_cb,
                .val_handle = &g_char_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }, // Characteristic list terminator
        },
    },
    { 0 }, // Service list terminator
};

// ─────────────────────────────────────────────────────────────────────────────
// GAP event handler
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief GAP event callback — handles all BLE connection lifecycle events.
 *
 * Called by NimBLE on the host task for every GAP event. Manages the
 * internal connection state flags and triggers re-advertising when needed.
 *
 * Handled events:
 *   - @c BLE_GAP_EVENT_CONNECT    — client connected or connection attempt failed
 *   - @c BLE_GAP_EVENT_DISCONNECT — client disconnected
 *   - @c BLE_GAP_EVENT_ADV_COMPLETE — advertising window expired
 *   - @c BLE_GAP_EVENT_MTU        — ATT MTU negotiated
 *   - @c BLE_GAP_EVENT_SUBSCRIBE  — client wrote to the CCCD
 *
 * @param event  Pointer to the GAP event structure.
 * @param arg    Optional user argument (unused).
 *
 * @return 0 always.
 */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0)
            {
                // Successful connection — mark connected but not yet subscribed.
                // BLE_is_connected() returns false until the client enables
                // notifications via the CCCD (BLE_GAP_EVENT_SUBSCRIBE).
                g_connected   = true;
                g_subscribed  = false;
                g_conn_handle = event->connect.conn_handle;
                g_mtu         = BLE_CHUNK_SIZE_MIN + 3; // Reset to default until negotiated
                ESP_LOGI(TAG, "Client connected — handle %d", g_conn_handle);
            }
            else
            {
                // Connection attempt failed — restart advertising so
                // the device remains discoverable
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
            // The advertising window expired (BLE_HS_FOREVER should prevent this,
            // but restart anyway as a safety net)
            ESP_LOGI(TAG, "Advertising complete, restarting");
            ble_advertise();
            break;

        case BLE_GAP_EVENT_MTU:
            // Client has requested a larger ATT MTU — update chunk size accordingly.
            // Larger MTU = fewer chunks per JSON payload = faster transmission.
            g_mtu = event->mtu.value;
            ESP_LOGI(TAG, "MTU negotiated: %d bytes (payload: %d bytes)",
                     g_mtu, g_mtu - 3);
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            // Client wrote to the Client Characteristic Configuration Descriptor (CCCD).
            // cur_notify == 1 means notifications are now enabled.
            // cur_notify == 0 means the client has unsubscribed.
            g_subscribed = (bool)event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Client %s notifications",
                     g_subscribed ? "subscribed to" : "unsubscribed from");
            break;

        default:
            break;
    }

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Advertising
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Start undirected connectable BLE advertising.
 *
 * Builds the advertising payload with the device name and general
 * discoverable flags, then starts advertising indefinitely
 * (BLE_HS_FOREVER) using a public address.
 *
 * Called automatically at startup via ble_on_sync(), and again whenever
 * the connection is lost or an advertising window expires.
 */
static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields  fields     = {0};

    // Include the device name in the advertising packet so scanners
    // can identify the device without connecting first
    fields.name             = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len         = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    // General discoverable mode, BR/EDR (Classic Bluetooth) not supported
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to set advertising fields: %d", rc);
        return;
    }

    // Undirected connectable — any scanner can initiate a connection
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to start advertising: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising as \"%s\"", BLE_DEVICE_NAME);
}

// ─────────────────────────────────────────────────────────────────────────────
// NimBLE host callbacks
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Called by NimBLE when the host stack has finished initialising
 *        and is ready to use.
 *
 * Verifies a valid Bluetooth address is available, then starts advertising.
 * Registered as ble_hs_cfg.sync_cb in BLE_init().
 */
static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to ensure address: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE stack synced — starting advertising");
    ble_advertise();
}

/**
 * @brief Called by NimBLE when the host stack resets due to a fatal error.
 *
 * Clears connection state so the system doesn't attempt to send data
 * on a stale connection handle after the stack recovers.
 * Registered as ble_hs_cfg.reset_cb in BLE_init().
 *
 * @param reason  NimBLE error code indicating why the reset occurred.
 */
static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE stack reset — reason %d", reason);
    g_connected  = false;
    g_subscribed = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// NimBLE host task
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief FreeRTOS task that runs the NimBLE host event loop.
 *
 * Calls nimble_port_run() which blocks indefinitely processing BLE host
 * events. Started by BLE_init() via nimble_port_freertos_init().
 * nimble_port_freertos_deinit() is called if the loop ever exits
 * (should not happen in normal operation).
 *
 * @param param  Unused task parameter.
 */
static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Initialise the NimBLE stack, register the GATT service, and start advertising.
 *
 * Initialisation sequence:
 *   1. Initialise NVS (required by NimBLE for bonding/config storage)
 *   2. Initialise the NimBLE port
 *   3. Register sync and reset callbacks
 *   4. Initialise GAP and GATT services
 *   5. Register the custom service and characteristic
 *   6. Set the device name
 *   7. Start the NimBLE host task — advertising begins when sync_cb fires
 *
 * @return 0 on success, non-zero NimBLE error code on failure.
 */
int BLE_init(void)
{
    // NimBLE requires NVS for persistent storage (e.g. bonding keys).
    // Erase and reinitialise if the partition is full or version has changed.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    int rc = nimble_port_init();
    if (rc != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return rc;
    }

    // Register callbacks so we know when the stack is ready (sync)
    // and when it has recovered from a fatal error (reset)
    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    // Initialise the standard GAP and GATT services required by NimBLE
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Count attribute handles needed for our service table and allocate them
    rc = ble_gatts_count_cfg(g_gatt_services);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return rc;
    }

    // Register the service — NimBLE populates g_char_val_handle here
    rc = ble_gatts_add_svcs(g_gatt_services);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return rc;
    }

    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to set device name: %d", rc);
        return rc;
    }

    // Start the NimBLE host task — ble_on_sync() fires when the stack is
    // ready and triggers advertising
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE init complete");
    return 0;
}

/**
 * @brief Check whether a client is connected and has subscribed to notifications.
 *
 * @return true  if a client is connected AND has subscribed to notifications.
 * @return false otherwise.
 */
bool BLE_is_connected(void)
{
    return g_connected && g_subscribed;
}

/**
 * @brief Send a JSON string to the connected BLE client as chunked notifications.
 *
 * Splits the JSON string into (MTU - 3) byte chunks and sends each one as
 * a GATT notification. After all chunks are sent, appends a single 0x04 (EOT)
 * byte so the client's PacketParser can detect end-of-message reliably.
 *
 * Each chunk send is retried up to 3 times with a 10ms delay to handle
 * transient mbuf pool exhaustion under load.
 *
 * @param json      Null-terminated JSON string to send.
 * @param json_len  Length of the JSON string in bytes (excluding null terminator).
 *
 * @return  0 on success.
 * @return -1 if no client is connected or notifications are not subscribed.
 * @return Non-zero NimBLE error code if a notification fails after all retries.
 */
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

    // ATT notifications carry 3 bytes of overhead (opcode + handle),
    // so the usable payload per chunk is (MTU - 3).
    // Fall back to BLE_CHUNK_SIZE_MIN if MTU is somehow below minimum.
    uint16_t payload_mtu = (g_mtu > 3) ? (g_mtu - 3) : BLE_CHUNK_SIZE_MIN;

    size_t offset = 0;

    while (offset < json_len)
    {
        size_t remaining = json_len - offset;
        size_t chunk_len = (remaining > payload_mtu) ? payload_mtu : remaining;

        // Retry up to 3 times to handle transient mbuf pool exhaustion.
        // mbufs are a fixed pool — if the stack hasn't freed the previous
        // ones yet, allocation will fail briefly before succeeding.
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

            ESP_LOGW(TAG, "Notify failed at offset %d (rc=%d), retrying...",
                     (int)offset, rc);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (rc != 0)
        {
            ESP_LOGE(TAG, "Notify failed after retries at offset %d: %d",
                     (int)offset, rc);
            return rc;
        }

        offset += chunk_len;

        // Brief inter-chunk delay gives the client time to process each
        // notification before the next one arrives
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Send the EOT sentinel as a standalone 1-byte notification.
    // The client's PacketParser treats this as the signal to flush its
    // buffer and parse the accumulated JSON.
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
    ESP_LOGI(TAG, "Sent %d bytes in %d chunks (MTU payload: %d)",
             (int)json_len, chunks, payload_mtu);

    return 0;
}