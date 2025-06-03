#include <stdio.h>
#include <string.h>
#include "ble_hal.h"

// --- Static Global Variables ---
static GDBusConnection* dbus_conn = NULL;       // D-Bus system bus connection
static GMainLoop* app_provided_loop = NULL;     // App-provided GMainLoop
static GMainLoop* internal_loop = NULL;         // HAL-created GMainLoop
static guint bluez_name_watch_id = 0;           // Watch ID for BlueZ service name
static guint object_manager_signal_watch_id = 0; // For InterfacesAdded/Removed
static BleHalAdapterInfo active_adapter;         // Store info about the selected/active adapter
static gboolean active_adapter_found = FALSE;

static BleHalConfig hal_global_config;          // Stored HAL configuration
static gboolean hal_initialized = FALSE;        // HAL initialization state

typedef struct {
    BleHalResultCb app_callback;
    void* app_user_data;
} SetPowerAsyncData;

void generic_result_cb(BleHalStatus error_code, void* user_data) {
    const char* operation_description = (const char*)user_data; // Cast user_data to its expected type

    if (operation_description == NULL) {
        operation_description = "Unknown operation";
    }

    if (error_code == BLE_HAL_SUCCESS) {
        printf("HAL_APP: Operation '%s' completed successfully.\n", operation_description);
    } else {
        fprintf(stderr, "HAL_APP: Operation '%s' failed with error code: %d\n", operation_description, error_code);
    }
}

static void on_object_manager_signal(GDBusConnection *connection,
                                     const gchar *sender_name,
                                     const gchar *object_path,
                                     const gchar *interface_name,
                                     const gchar *signal_name,
                                     GVariant *parameters,
                                     gpointer user_data);

static void process_adapter_interface(const gchar* object_path, GVariant* interface_properties);

static void initial_object_scan(void);

// D-Bus name watcher callback forward declarations
static void on_bluez_appeared(GDBusConnection *connection, const gchar *name, const gchar *name_owner, gpointer user_data);
static void on_bluez_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data);

// --- D-Bus Name Watcher Callbacks ---

/**
 * @brief Handles D-Bus ObjectManager signals (InterfacesAdded/Removed) from org.bluez.
 * Used for dynamic discovery of adapters and other BlueZ objects.
 */
static void on_object_manager_signal(GDBusConnection *connection,
                                     const gchar *sender_name,
                                     const gchar *object_path_param,    // Path of the ObjectManager emitting the signal (usually "/")
                                     const gchar *interface_name_signal,
                                     const gchar *signal_name,          // "InterfacesAdded" or "InterfacesRemoved"
                                     GVariant *parameters,
                                     gpointer user_data) {
    // Only process signals from org.bluez
    if (g_strcmp0(sender_name, "org.bluez") != 0) {
        return;
    }

    // The actual object path is within the 'parameters' GVariant.
    if (g_strcmp0(signal_name, "InterfacesAdded") == 0) {
        const gchar *actual_object_path;
        GVariant *interfaces_and_properties; // Dict of interfaces and their properties for the added object

        g_variant_get(parameters, "(&oa{sa{sv}})", &actual_object_path, &interfaces_and_properties);
        printf("HAL: InterfacesAdded for object %s\n", actual_object_path);

        GVariantIter iter;
        const gchar *interface_name; // e.g., org.bluez.Adapter1
        GVariant *properties;

        g_variant_iter_init(&iter, interfaces_and_properties);
        while (g_variant_iter_next(&iter, "{&s@a{sv}}", &interface_name, &properties)) {
            if (g_strcmp0(interface_name, "org.bluez.Adapter1") == 0) {
                if (!active_adapter_found) { // Use the first adapter discovered
                     process_adapter_interface(actual_object_path, properties);
                } else {
                    printf("HAL: Ignoring newly added adapter %s (one already active).\n", actual_object_path);
                }
            }
            // TODO: Handle "org.bluez.Device1" here for device discovery later.
            g_variant_unref(properties);
        }
        g_variant_unref(interfaces_and_properties);

    } else if (g_strcmp0(signal_name, "InterfacesRemoved") == 0) {
        const gchar *actual_object_path;
        GVariant *interfaces_array; // Array of interface name strings that were removed

        g_variant_get(parameters, "(&oas)", &actual_object_path, &interfaces_array);
        printf("HAL: InterfacesRemoved for object %s\n", actual_object_path);

        // Check if the removed object was our active adapter
        if (active_adapter_found && g_strcmp0(actual_object_path, active_adapter.path) == 0) {
            GVariantIter iter;
            const gchar *removed_interface_name;
            g_variant_iter_init(&iter, interfaces_array);
            while (g_variant_iter_next(&iter, "&s", &removed_interface_name)) {
                if (g_strcmp0(removed_interface_name, "org.bluez.Adapter1") == 0) {
                    printf("HAL: Active adapter %s was removed.\n", active_adapter.path);
                    active_adapter_found = FALSE;
                    memset(&active_adapter, 0, sizeof(BleHalAdapterInfo));
                    // TODO: Notify application or try to find another adapter.
                    break;
                }
            }
            g_variant_unref(interfaces_array);
        }
        // TODO: Handle removal of other object types (devices, etc.) if tracking them.
    }
}

/**
 * @brief Called when org.bluez D-Bus service is available.
 */
static void on_bluez_appeared(GDBusConnection *connection, // This 'connection' parameter is the specific connection where the name appeared.
                                                          // It should be the same as your global 'dbus_conn'.
                              const gchar *name,
                              const gchar *name_owner,
                              gpointer user_data) {
    printf("HAL: BlueZ service (%s owner: %s) appeared.\n", name, name_owner);

    if (object_manager_signal_watch_id > 0) {
        // Already subscribed, perhaps BlueZ restarted. Clean up old just in case.
        // Ensure dbus_conn is valid if you're using the global one here.
        if (dbus_conn) {
             g_dbus_connection_signal_unsubscribe(dbus_conn, object_manager_signal_watch_id);
        }
        object_manager_signal_watch_id = 0;
    }

    // Reset adapter state on BlueZ appearance
    active_adapter_found = FALSE;
    memset(&active_adapter, 0, sizeof(BleHalAdapterInfo));
    printf("HAL: Active adapter state reset.\n"); // Added a log for clarity

    if (dbus_conn) { // Ensure dbus_conn is not NULL
        object_manager_signal_watch_id = g_dbus_connection_signal_subscribe(
            dbus_conn,                                  // The D-Bus connection
            "org.bluez",                                // Sender name to watch
            "org.freedesktop.DBus.ObjectManager",       // Interface name
            NULL,                                       // Member (signal name, NULL for all signals from this interface)
            "/",                                        // Object path (subscribe to root for all BlueZ objects)
            NULL,                                       // Arg0 filter (NULL for any)
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_object_manager_signal,                   // Your callback function for these signals
            NULL,                                       // User data for the callback
            NULL);                                      // User data free function

        if (object_manager_signal_watch_id == 0) {
            fprintf(stderr, "HAL Error: Failed to subscribe to ObjectManager signals.\n");
        } else {
            printf("HAL: Subscribed to ObjectManager signals (watch ID: %u).\n", object_manager_signal_watch_id);
            // Perform an initial scan for existing objects ONLY IF subscription was successful
            initial_object_scan();
        }
    } else {
        fprintf(stderr, "HAL Error: dbus_conn is NULL in on_bluez_appeared, cannot subscribe to signals.\n");
    }

    // Notify the application that the BlueZ service is up
    if (hal_global_config.global_event_cb) {
        BleHalEventData event_data = {0};
        hal_global_config.global_event_cb(BLE_HAL_EVENT_BLUEZ_SERVICE_UP, &event_data, hal_global_config.global_event_user_data);
    }
}

/**
 * @brief Called when org.bluez D-Bus service disappears.
 */
static void on_bluez_vanished(GDBusConnection *connection,
                              const gchar *name,
                              gpointer user_data) {
    printf("HAL: BlueZ service (%s) vanished.\n", name);

    // Unsubscribe from ObjectManager signals if we were subscribed
    if (object_manager_signal_watch_id > 0 && dbus_conn) { // Ensure dbus_conn is still valid for unsubscription
        g_dbus_connection_signal_unsubscribe(dbus_conn, object_manager_signal_watch_id);
        object_manager_signal_watch_id = 0; // Reset the watch ID
        printf("HAL: Unsubscribed from ObjectManager signals.\n");
    }

    // Clear active adapter information
    if (active_adapter_found) {
        active_adapter_found = FALSE;
        memset(&active_adapter, 0, sizeof(BleHalAdapterInfo)); // Clear the struct
        printf("HAL: Cleared active adapter info.\n");
    }

    // Notify the application
    if (hal_global_config.global_event_cb) {
        BleHalEventData event_data = {0};
        hal_global_config.global_event_cb(BLE_HAL_EVENT_BLUEZ_SERVICE_DOWN, &event_data, hal_global_config.global_event_user_data);
    }
}

/**
 * @brief Processes properties for a discovered org.bluez.Adapter1 interface.
 */
static void process_adapter_interface(const gchar* object_path, GVariant* properties) {
    if (active_adapter_found) {
        // For simplicity, we'll just use the first adapter found.
        // More sophisticated logic could allow selecting or managing multiple.
        printf("HAL: Already have an active adapter (%s), ignoring new one: %s\n", active_adapter.path, object_path);
        return;
    }

    printf("HAL: Found potential adapter at %s\n", object_path);
    BleHalAdapterInfo new_adapter_info = {0};
    strncpy(new_adapter_info.path, object_path, sizeof(new_adapter_info.path) - 1);

    GVariantIter iter;
    const gchar *prop_name;
    GVariant *prop_value;

    g_variant_iter_init(&iter, properties);
    while (g_variant_iter_next(&iter, "{&sv}", &prop_name, &prop_value)) {
        if (g_strcmp0(prop_name, "Address") == 0 && g_variant_is_of_type(prop_value, G_VARIANT_TYPE_STRING)) {
            strncpy(new_adapter_info.address, g_variant_get_string(prop_value, NULL), sizeof(new_adapter_info.address) - 1);
        } else if (g_strcmp0(prop_name, "Name") == 0 && g_variant_is_of_type(prop_value, G_VARIANT_TYPE_STRING)) {
            strncpy(new_adapter_info.name, g_variant_get_string(prop_value, NULL), sizeof(new_adapter_info.name) - 1);
        } else if (g_strcmp0(prop_name, "Powered") == 0 && g_variant_is_of_type(prop_value, G_VARIANT_TYPE_BOOLEAN)) {
            new_adapter_info.powered = g_variant_get_boolean(prop_value);
        }
        // Add more properties as needed from org.bluez.Adapter1
        g_variant_unref(prop_value);
    }

    // Basic check if we got essential info
    if (strlen(new_adapter_info.address) > 0) {
        active_adapter = new_adapter_info;
        active_adapter_found = TRUE;
        printf("HAL: Configured active adapter: %s, Address: %s, Name: %s, Powered: %s\n",
               active_adapter.path, active_adapter.address, active_adapter.name,
               active_adapter.powered ? "on" : "off");

        // TODO: Notify application about adapter readiness or proceed with next steps
        // (e.g., if auto-scan on adapter ready is a feature)

        // If the adapter is not powered, the application might need to be notified,
        // or the HAL could attempt to power it on if that's desired functionality.
        // Example: ble_hal_power_on_adapter(active_adapter.path); (to be implemented)
        if (active_adapter_found && !active_adapter.powered) {
            printf("HAL App: Adapter %s is not powered on. Attempting to power on...\n", active_adapter.address);
            ble_hal_set_adapter_power(active_adapter.path, TRUE, generic_result_cb, "SetPowerOn");
        }
    } else {
        printf("HAL: Adapter at %s did not have an address, not using.\n", object_path);
    }
}

/**
 * @brief Callback for GetManagedObjects D-Bus method.
 */
static void on_get_managed_objects_reply(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    GVariant *result_tuple = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), res, &error);

    if (error) {
        fprintf(stderr, "HAL Error: GetManagedObjects failed: %s\n", error->message);
        g_error_free(error);
        return;
    }

    if (result_tuple) {
        GVariant *dict_entries = g_variant_get_child_value(result_tuple, 0); // Get the a{oa{sa{sv}}}
        GVariantIter iter_obj_paths;
        const gchar *object_path;
        GVariant *ifaces_and_props_dict; // This is a{sa{sv}}

        g_variant_iter_init(&iter_obj_paths, dict_entries);
        printf("HAL: Processing GetManagedObjects reply...\n");
        while (g_variant_iter_next(&iter_obj_paths, "{&o@a{sa{sv}}}", &object_path, &ifaces_and_props_dict)) {
            GVariantIter iter_ifaces;
            const gchar *iface_name;
            GVariant *props_dict; // This is a{sv}

            g_variant_iter_init(&iter_ifaces, ifaces_and_props_dict);
            while (g_variant_iter_next(&iter_ifaces, "{&s@a{sv}}", &iface_name, &props_dict)) {
                if (g_strcmp0(iface_name, "org.bluez.Adapter1") == 0) { //
                    if (!active_adapter_found) { // Process first one found
                        process_adapter_interface(object_path, props_dict);
                    }
                }
                // Here you could also process org.bluez.Device1 if needed at this stage
                g_variant_unref(props_dict);
            }
            g_variant_unref(ifaces_and_props_dict);
        }
        g_variant_unref(dict_entries);
        g_variant_unref(result_tuple);
    }

    if (!active_adapter_found) {
        printf("HAL: No Bluetooth adapter found after initial scan of managed objects.\n");
    }
}

/**
 * @brief Performs an initial scan of D-Bus objects from BlueZ.
 */
static void initial_object_scan(void) {
    if (!dbus_conn) return;

    printf("HAL: Performing initial scan for BlueZ managed objects.\n");
    g_dbus_connection_call(dbus_conn,
                           "org.bluez",                             // Bus name
                           "/",                                     // Object path for ObjectManager
                           "org.freedesktop.DBus.ObjectManager",    // Interface
                           "GetManagedObjects",                     // Method
                           NULL,                                    // Parameters
                           G_VARIANT_TYPE("(a{oa{sa{sv}}})"),      // Reply type
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,                                      // Timeout
                           NULL,                                    // GCancellable
                           on_get_managed_objects_reply,            // Callback
                           NULL);                                   // User data
}

// --- Public API Functions ---

BleHalStatus ble_hal_init(const BleHalConfig* config, GMainLoop* loop) {
    GError *error = NULL;

    if (hal_initialized) {
        printf("HAL: Already initialized.\n");
        return BLE_HAL_SUCCESS;
    }

    if (!config) {
        printf("HAL Error: Null configuration.\n");
        return BLE_HAL_ERROR_INVALID_PARAMS;
    }
    hal_global_config = *config; // Store config

    printf("HAL: Initializing...\n");

    dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error); // Connect to system D-Bus
    if (error) {
        fprintf(stderr, "HAL Error: D-Bus connection failed: %s\n", error->message);
        g_error_free(error);
        return BLE_HAL_ERROR_DBUS;
    }
    if (!dbus_conn) {
        fprintf(stderr, "HAL Error: D-Bus connection failed (null conn, no GError).\n");
        return BLE_HAL_ERROR_DBUS;
    }
    printf("HAL: D-Bus connection acquired.\n");

    if (loop) {
        app_provided_loop = loop; // Use app's GMainLoop
        printf("HAL: Using application-provided GMainLoop.\n");
    } else {
        internal_loop = g_main_loop_new(NULL, FALSE); // Create internal GMainLoop
        printf("HAL: Created internal GMainLoop (app must manage its execution).\n");
    }

    // Watch for BlueZ service on D-Bus
    bluez_name_watch_id = g_bus_watch_name(
        G_BUS_TYPE_SYSTEM, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_bluez_appeared, on_bluez_vanished,
        NULL, NULL);

    if (bluez_name_watch_id == 0) {
        fprintf(stderr, "HAL Error: Failed to watch BlueZ D-Bus name.\n");
        g_object_unref(dbus_conn);
        dbus_conn = NULL;
        if (internal_loop) {
            g_main_loop_unref(internal_loop);
            internal_loop = NULL;
        }
        return BLE_HAL_ERROR_DBUS;
    }
    printf("HAL: Watching BlueZ D-Bus service (ID: %u).\n", bluez_name_watch_id);

    hal_initialized = TRUE;
    printf("HAL: Initialization successful.\n");
    return BLE_HAL_SUCCESS;
}

void ble_hal_deinit(void) {
    if (!hal_initialized) {
        printf("HAL: Not initialized or already deinitialized.\n");
        return;
    }
    printf("HAL: Deinitializing...\n");

    if (bluez_name_watch_id > 0) {
        g_bus_unwatch_name(bluez_name_watch_id); // Stop watching BlueZ name
        bluez_name_watch_id = 0;
        printf("HAL: Stopped watching BlueZ D-Bus service.\n");
    }

    if (dbus_conn) {
        g_object_unref(dbus_conn); // Close D-Bus connection
        dbus_conn = NULL;
        printf("HAL: D-Bus connection closed.\n");
    }

    if (internal_loop) { // Clean up internal GMainLoop
        if (g_main_loop_is_running(internal_loop)) {
            g_main_loop_quit(internal_loop);
        }
        g_main_loop_unref(internal_loop);
        internal_loop = NULL;
        printf("HAL: Internal GMainLoop cleaned up.\n");
    }
    app_provided_loop = NULL;

    memset(&hal_global_config, 0, sizeof(BleHalConfig)); // Reset global config

    hal_initialized = FALSE;
    printf("HAL: Deinitialization complete.\n");
}

static void on_set_power_reply(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    SetPowerAsyncData *data = (SetPowerAsyncData *)user_data;
    GError *error = NULL;
    BleHalStatus hal_err = BLE_HAL_SUCCESS;

    g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), res, &error);

    if (error) {
        fprintf(stderr, "HAL Error: Failed to set 'Powered' property: %s (D-Bus error: %s)\n",
                error->message, g_dbus_error_get_remote_error(error));
        // TODO: Map GError to BleHalError more specifically if desired
        // For example, check g_dbus_error_get_remote_error(error) for BlueZ specific errors.
        hal_err = BLE_HAL_ERROR_DBUS;
        g_error_free(error);
    } else {
        printf("HAL: 'Powered' property set successfully.\n");
        // Note: The actual state change confirmation usually comes via a PropertiesChanged signal
        // for the 'Powered' property on org.bluez.Adapter1. Listening to this signal
        // would be the robust way to confirm the power state has indeed changed.
    }

    if (data->app_callback) {
        data->app_callback(hal_err, data->app_user_data);
    }
    g_free(data);
}

BleHalStatus ble_hal_set_adapter_power(const char* adapter_path, gboolean power_on, BleHalResultCb cb, void* user_data) {
    if (!hal_initialized || !dbus_conn) {
        fprintf(stderr, "HAL Error: HAL not initialized or D-Bus connection lost.\n");
        if (cb) cb(BLE_HAL_ERROR_NOT_INITIALIZED, user_data); // Call immediately with error
        return BLE_HAL_ERROR_NOT_INITIALIZED;
    }

    if (!adapter_path) {
        fprintf(stderr, "HAL Error: Adapter path cannot be NULL for set_adapter_power.\n");
        if (cb) cb(BLE_HAL_ERROR_INVALID_PARAMS, user_data);
        return BLE_HAL_ERROR_INVALID_PARAMS;
    }

    // The 'Powered' property expects a GVariant of type boolean ('b').
    GVariant *value_variant = g_variant_new_boolean(power_on);

    // The parameters for DBus.Properties.Set are:
    // interface_name (s), property_name (s), value (v)
    GVariant *params = g_variant_new("(ssv)",
                                     "org.bluez.Adapter1", // Interface name
                                     "Powered",            // Property name
                                     value_variant);       // The new value (GVariant takes ownership of value_variant)

    SetPowerAsyncData *async_data = g_new(SetPowerAsyncData, 1);
    async_data->app_callback = cb;
    async_data->app_user_data = user_data;

    printf("HAL: Attempting to set 'Powered' property to %s for adapter %s\n", power_on ? "ON" : "OFF", adapter_path);

    g_dbus_connection_call(dbus_conn,
                           "org.bluez",                             // D-Bus service name
                           adapter_path,                          // Object path of the adapter
                           "org.freedesktop.DBus.Properties",       // Interface name for property operations
                           "Set",                                   // Method name
                           params,                                  // Parameters (GVariant owns its children)
                           NULL,                                    // Reply type (Set has no specific reply value other than error)
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,                                      // Timeout
                           NULL,                                    // GCancellable
                           on_set_power_reply,                      // Callback for the reply
                           async_data);                             // User data for the callback

    return BLE_HAL_PENDING; // Operation is asynchronous
}
