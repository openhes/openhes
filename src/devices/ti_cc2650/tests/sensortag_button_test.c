////////////////////////////////////////////////////////////////////////////////
// Copyright 2026 Tom G. Huang <tomghuang@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Button reader for the CC2650 SensorTag, over BlueZ D-Bus.
///
/// @details
/// Handles the full lifecycle: connect -> pair -> discover -> notify -> cleanup,
/// with no dependency on bluetoothctl or external pairing.
///
/// This is the standalone program the device-layer accessor sensortag_button.c
/// was extracted from -- and the source of the known-good button characteristic
/// path suffix that src/han/ble/hes_ble_bluez.c subscribes to. It needs real
/// hardware -- a Bluetooth adapter, a paired/trusted tag and a running BlueZ -- so
/// it is not part of `make all` and not a ctest test; build it with
/// -DOPENHES_BUILD_DEVICE_TESTS=ON.
///
/// MAC and DEVICE_PATH below select which tag to talk to.

#include <gio/gio.h>
#include <glib.h>

#include <signal.h>
#include <stdio.h>

#define BLUEZ_BUS_NAME "org.bluez"
#define DEVICE_INTERFACE "org.bluez.Device1"
#define CHAR_INTERFACE "org.bluez.GattCharacteristic1"
#define ADAPTER_PATH "/org/bluez/hci0"

#define MAC "54:6C:0E:B7:20:04"
#define DEVICE_PATH "/org/bluez/hci0/dev_54_6C_0E_B7_20_04"

/// Known characteristic for TI SensorTag button notifications
#define BUTTON_CHAR_PATH DEVICE_PATH "/service004a/char004b"

static GMainLoop* loop = NULL;
static GDBusProxy* char_proxy = NULL;
static GDBusProxy* dev_proxy = NULL;
static GDBusConnection* connection = NULL;

////////////////////////////////////////////////////////////////////////////////
/// SIGINT/SIGTERM handler: quits the main loop, so the program leaves through
/// cleanup() instead of being killed mid-transfer.
///
/// @param sig The signal number (unused).
static void handle_sigint(int sig)
{
    (void)sig;
    if (loop && g_main_loop_is_running(loop)) {
        g_main_loop_quit(loop);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Notify callback for the button characteristic: prints which buttons are
/// pressed. The notify byte packs LEFT(0x01) / RIGHT(0x02) / REED(0x04), so a
/// set bit means that key is down and 0 means all released.
///
/// @param proxy                 The characteristic proxy (unused).
/// @param changed_properties    The properties that changed.
/// @param invalidated_properties Those that were invalidated (unused).
/// @param user_data             Unused.
static void on_properties_changed(GDBusProxy* proxy,
                                  GVariant* changed_properties,
                                  GStrv invalidated_properties,
                                  gpointer user_data)
{
    (void)proxy;
    (void)invalidated_properties;
    (void)user_data;

    GVariant* value_variant = g_variant_lookup_value(changed_properties, "Value", NULL);
    if (!value_variant) {
        return;
    }

    GVariantIter* iter;
    g_variant_get(value_variant, "ay", &iter);

    guchar state_byte = 0;
    if (g_variant_iter_next(iter, "y", &state_byte)) {
        if (state_byte == 0) {
            printf("-> [RELEASED] All buttons let go.\n");
        } else {
            printf("-> [PRESSED] ");
            if (state_byte & 0x01) {
                printf("LEFT Button ");
            }
            if (state_byte & 0x02) {
                printf("RIGHT Button ");
            }
            if (state_byte & 0x04) {
                printf("REED Switch ");
            }
            printf("\n");
        }
    }
    fflush(stdout);

    g_variant_iter_free(iter);
    g_variant_unref(value_variant);
}

////////////////////////////////////////////////////////////////////////////////
/// Waits until BlueZ reports ServicesResolved for the device, iterating the main
/// context while it waits.
///
/// Iterating matters here in particular: the property is only set when the D-Bus
/// notifications that arrive during the wait are processed, so a plain sleep loop
/// would never see it flip from FALSE to TRUE.
///
/// @param dproxy      The device proxy to watch.
/// @param timeout_sec How long to wait, in seconds (polled every 250 ms).
/// @return TRUE when services were resolved, FALSE on timeout.
static gboolean wait_for_services_resolved(GDBusProxy* dproxy, int timeout_sec)
{
    // Fast path: already resolved
    GVariant* val = g_dbus_proxy_get_cached_property(dproxy, "ServicesResolved");
    if (val) {
        gboolean resolved = g_variant_get_boolean(val);
        g_variant_unref(val);
        if (resolved) {
            return TRUE;
        }
    }

    // Poll: iterate the main context so D-Bus messages get processed
    for (int i = 0; i < timeout_sec * 4; i++) {
        // Process any pending D-Bus events so the property cache updates
        g_main_context_iteration(NULL, FALSE);
        g_usleep(250000);

        val = g_dbus_proxy_get_cached_property(dproxy, "ServicesResolved");
        if (val) {
            gboolean resolved = g_variant_get_boolean(val);
            g_variant_unref(val);
            if (resolved) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
/// Shuts down in the right order: stops the notifications, releases the proxies,
/// and disconnects the device.
static void cleanup(void)
{
    printf("\nCleaning up...\n");

    // Stop notifications
    if (char_proxy) {
        g_dbus_proxy_call_sync(char_proxy, "StopNotify", NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
                               NULL);
        g_object_unref(char_proxy);
        char_proxy = NULL;
    }

    // Disconnect from the device
    if (dev_proxy) {
        g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                               NULL);
        g_object_unref(dev_proxy);
        dev_proxy = NULL;
    }

    if (connection) {
        g_object_unref(connection);
    }

    if (loop) {
        g_main_loop_unref(loop);
    }

    printf("Disconnected. Exiting.\n");
}

// -------------------------------------------------------------------------
int main(void)
{
    GError* error = NULL;

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    // ---- 1. Connect to system D-Bus ----
    connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        g_printerr("D-Bus connection failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    // ---- 2. Create device proxy ----
    dev_proxy = g_dbus_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME,
                                      DEVICE_PATH, DEVICE_INTERFACE, NULL, &error);
    if (!dev_proxy) {
        g_printerr(
                "Device proxy failed. Is the MAC correct?\n"
                "  Try: bluetoothctl scan on  (then check device path)\n"
                "  Error: %s\n",
                error->message);
        g_error_free(error);
        g_object_unref(connection);
        return 1;
    }

    // ---- 3. Connect (pairs automatically if needed) ----
    printf("Connecting to SensorTag %s...\n", MAC);
    g_dbus_proxy_call_sync(dev_proxy, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);
    if (error) {
        g_printerr(
                "Connect failed: %s\n"
                "Make sure the SensorTag is nearby and advertising.\n",
                error->message);
        g_error_free(error);
        g_object_unref(dev_proxy);
        g_object_unref(connection);
        return 1;
    }
    printf("Connected. Waiting for services to resolve...\n");

    if (!wait_for_services_resolved(dev_proxy, 30)) {
        g_printerr("Services did not resolve within 30 s.\n");
        // Disconnect so BlueZ doesn't linger in a half-connected state
        g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                               NULL);
        g_object_unref(dev_proxy);
        g_object_unref(connection);
        return 1;
    }
    printf("Services resolved.\n");

    // ---- 4. Create characteristic proxy and start notifications ----
    char_proxy = g_dbus_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME,
                                       BUTTON_CHAR_PATH, CHAR_INTERFACE, NULL, &error);
    if (!char_proxy) {
        g_printerr(
                "Characteristic proxy failed.\n"
                "  Path: %s\n"
                "  Error: %s\n",
                BUTTON_CHAR_PATH, error->message);
        g_error_free(error);
        // Don't return -- still try to clean up
    } else {
        // Connect signal
        g_signal_connect(char_proxy, "g-properties-changed", G_CALLBACK(on_properties_changed),
                         NULL);

        // StartNotify
        printf("Subscribing to button notifications...\n");
        GVariant* reply = g_dbus_proxy_call_sync(char_proxy, "StartNotify", NULL,
                                                 G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
        if (!reply) {
            g_printerr("StartNotify failed: %s\n", error ? error->message : "unknown");
            if (error) {
                g_error_free(error);
            }
        } else {
            g_variant_unref(reply);
        }
    }

    printf("\nReady! Press/release SensorTag buttons (Ctrl+C to quit).\n");

    // ---- 5. Run event loop until Ctrl+C ----
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    // ---- 6. Clean shutdown ----
    cleanup();
    return 0;
}
