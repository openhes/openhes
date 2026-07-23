#include <stdio.h>
#include <glib.h>
#include <gio/gio.h>

// REPLACE with your exact CC2650 SensorTag MAC address, using UNDERSCORES instead of colons
#define MAC_ADDR "54_6C_0E_B7_20_04"

// BlueZ D-Bus object definitions
#define BLUEZ_BUS_NAME "org.bluez"
#define BUTTON_CHAR_PATH "/org/bluez/hci0/dev_" MAC_ADDR "/service004a/char004b"
#define CHAR_INTERFACE "org.bluez.GattCharacteristic1"

static GMainLoop *loop = NULL;

// Callback function triggered instantly when the button status changes
static void on_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
                                  GStrv invalidated_properties, gpointer user_data) {
    GVariant *value_variant = g_variant_lookup_value(changed_properties, "Value", NULL);
    if (!value_variant) {
        return;
    }

    // Unpack the byte array array sent from the SensorTag
    GVariantIter *iter;
    g_variant_get(value_variant, "ay", &iter);

    guchar state_byte = 0;
    if (g_variant_iter_next(iter, "y", &state_byte)) {
        gboolean left_pressed  = (state_byte & 0x01) ? TRUE : FALSE;
        gboolean right_pressed = (state_byte & 0x02) ? TRUE : FALSE;
        gboolean reed_active   = (state_byte & 0x04) ? TRUE : FALSE;

        if (state_byte == 0) {
            printf("-> [RELEASED] All buttons let go.\n");
        } else {
            printf("-> [PRESSED] ");
            if (left_pressed)  printf("LEFT Button ");
            if (right_pressed) printf("RIGHT Button ");
            if (reed_active)   printf("REED Switch ");
            printf("\n");
        }
    }

    g_variant_iter_free(iter);
    g_variant_unref(value_variant);
}

int main() {
    GError *error = NULL;

    // Connect to the system D-Bus
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        g_printerr("Failed to connect to D-Bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    // Create a D-Bus proxy object for the Button Characteristic
    GDBusProxy *proxy = g_dbus_proxy_new_sync(
        connection,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        BLUEZ_BUS_NAME,
        BUTTON_CHAR_PATH,
        CHAR_INTERFACE,
        NULL,
        &error
    );

    if (!proxy) {
        g_printerr("Failed to create GATT proxy path: %s\n", error->message);
        g_error_free(error);
        g_object_unref(connection);
        return 1;
    }

    // Start a main execution context event loop
    loop = g_main_loop_new(NULL, FALSE);

    // Connect the properties-changed signal to our callback handler
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(on_properties_changed), NULL);

    // Call BlueZ 'StartNotify' method to subscribe to asynchronous notifications
    printf("Subscribing to button notification events...\n");
    GVariant *reply = g_dbus_proxy_call_sync(
        proxy,
        "StartNotify",
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (!reply) {
        g_printerr("Failed to enable button notifications: %s\n", error->message);
        g_error_free(error);
        g_object_unref(proxy);
        g_object_unref(connection);
        return 1;
    }
    g_variant_unref(reply);

    printf("Ready! Press or release the side buttons on the SensorTag.\n");
    printf("Press Ctrl+C inside this terminal to exit.\n");

    // Run the main event loop
    g_main_loop_run(loop);

    // Clean up allocations on close
    g_object_unref(proxy);
    g_object_unref(connection);
    g_main_loop_unref(loop);

    return 0;
}
