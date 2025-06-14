#ifndef BLE_HAL_H_
#define BLE_HAL_H_

#include <glib.h>
#include <gio/gio.h>

// --- Status Codes ---
typedef enum {
    BLE_HAL_SUCCESS = 0,                // Operation successful
    BLE_HAL_ERROR,                      // Generic error
    BLE_HAL_ERROR_DBUS,                 // D-Bus related error
    BLE_HAL_ERROR_NOT_INITIALIZED,      // HAL not initialized
    BLE_HAL_ERROR_INVALID_PARAMS,       // Invalid parameters provided
    BLE_HAL_PENDING                     // Asynchronous operation pending
} BleHalStatus;

// --- Global HAL Events ---
typedef enum {
    BLE_HAL_EVENT_BLUEZ_SERVICE_UP,     // BlueZ service is available
    BLE_HAL_EVENT_BLUEZ_SERVICE_DOWN,   // BlueZ service is not available
    // Add other global events
} BleHalEvent;

// --- Event Data ---
typedef struct {
    void* data; // Event-specific data payload
} BleHalEventData;

// --- Adapter Information ---
typedef struct {
    char path[256];         // D-Bus object path (e.g., /org/bluez/hci0)
    char address[18];       // XX:XX:XX:XX:XX:XX format
    char name[249];         // Adapter's Bluetooth name
    gboolean powered;       // Adapter's power state
    // Add other fields as needed, e.g., discovering status
} BleHalAdapterInfo;

// --- Configuration ---
typedef struct {
    // Callback for global HAL events (e.g., BlueZ service status)
    void (*global_event_cb)(BleHalEvent event_type, BleHalEventData* data, void* user_data);
    void* global_event_user_data;   // User data for global_event_cb
    // Other config options (e.g., log level)
} BleHalConfig;


typedef void (*BleHalResultCb)(BleHalStatus error, void* user_data);

/**
 * @brief Initializes the BLE HAL.
 * Connects to D-Bus, monitors BlueZ service.
 * @param config HAL configuration.
 * @param loop Optional GMainLoop for integration. If NULL, HAL may create its own.
 * @return BleHalStatus indicating success/failure.
 */
BleHalStatus ble_hal_init(const BleHalConfig* config, GMainLoop* loop);

/**
 * @brief Deinitializes the BLE HAL.
 * Closes D-Bus connection and cleans up resources.
 */
void ble_hal_deinit(void);

/**
 * @brief Sets the power state of the specified Bluetooth adapter.
 *
 * @param adapter_path The D-Bus object path of the adapter (e.g., "/org/bluez/hci0").
 * @param power_on TRUE to power on, FALSE to power off.
 * @param cb Callback function to be invoked with the result of the operation.
 * @param user_data User data to be passed to the callback.
 * @return BleHalStatus BLE_HAL_PENDING if the operation was initiated, or an error code.
 */
BleHalStatus ble_hal_set_adapter_power(const char* adapter_path, gboolean power_on, BleHalResultCb cb, void* user_data);

#endif // BLE_HAL_H_
