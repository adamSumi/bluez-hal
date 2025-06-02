#include <stdio.h>
#include <glib.h>
#include "ble_hal.h" // Assuming this path is correct based on your -Iinclude flag

// Simple global event callback for the sample app
void sample_global_event_cb(BleHalEvent event_type, BleHalEventData* data, void* user_data) {
    printf("HAL App: Received global HAL event: %d\n", event_type);
    if (event_type == BLE_HAL_EVENT_BLUEZ_SERVICE_UP) {
        printf("HAL App: BlueZ service is UP.\n");
    } else if (event_type == BLE_HAL_EVENT_BLUEZ_SERVICE_DOWN) {
        printf("HAL App: BlueZ service is DOWN.\n");
    }
}

int main(int argc, char *argv[]) {
    GMainLoop *main_loop = NULL;
    BleHalConfig hal_config;
    BleHalStatus status;

    printf("HAL App: Starting...\n");

    // Create a GMainLoop (as the HAL might use it or integrate with it)
    main_loop = g_main_loop_new(NULL, FALSE);
    if (!main_loop) {
        fprintf(stderr, "HAL App: Failed to create GMainLoop.\n");
        return 1;
    }

    // Prepare HAL configuration
    hal_config.global_event_cb = sample_global_event_cb;
    hal_config.global_event_user_data = NULL; // No specific user data for this simple example

    // Initialize the BLE HAL
    // We pass NULL for the loop parameter to let the HAL create its own internal one for this test.
    // Or, you could pass 'main_loop' if you intend the HAL to use this app's loop directly.
    status = ble_hal_init(&hal_config, NULL /* or pass main_loop */);
    if (status != BLE_HAL_SUCCESS) {
        fprintf(stderr, "HAL App: Failed to initialize BLE HAL, error: %d\n", status);
        g_main_loop_unref(main_loop);
        return 1;
    }

    printf("HAL App: BLE HAL initialized. Running GMainLoop for 5 seconds (or press Ctrl+C)...\n");
    // Run the main loop for a bit to allow D-Bus events (like BlueZ service appearing)
    // In a real app, this would run until the application quits.
    // Here, we use a timeout for a simple non-blocking test, or you can just run and Ctrl+C.
    g_timeout_add_seconds(5, (GSourceFunc)g_main_loop_quit, main_loop); // Quit after 5 seconds
    g_main_loop_run(main_loop);

    printf("HAL App: GMainLoop finished. Deinitializing BLE HAL...\n");

    // Deinitialize the BLE HAL
    ble_hal_deinit();

    // Clean up GMainLoop
    g_main_loop_unref(main_loop);

    printf("HAL App: Finished.\n");
    return 0;
}
