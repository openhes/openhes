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
/// @brief 9-axis motion (MPU-9250) reader for the CC2650 SensorTag, over BlueZ
/// D-Bus.
///
/// @details
/// Discovers the motion service characteristics by UUID, enables gyroscope,
/// accelerometer and magnetometer, and streams all 9 axes every 500 ms.
///
/// This is the standalone program the device-layer accessor sensortag_motion.c was
/// extracted from (see ../README.md). It needs real hardware -- a Bluetooth
/// adapter, a paired/trusted tag and a running BlueZ -- so it is not part of
/// `make all` and not a ctest test; build it with -DOPENHES_BUILD_DEVICE_TESTS=ON.
///
/// MAC and DEVICE_PATH below select which tag to talk to.

#include <gio/gio.h>
#include <glib.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLUEZ_BUS_NAME "org.bluez"
#define DEVICE_INTERFACE "org.bluez.Device1"
#define CHAR_INTERFACE "org.bluez.GattCharacteristic1"
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define ADAPTER_PATH "/org/bluez/hci0"

#define MAC "54:6C:0E:B7:20:04"
#define DEVICE_PATH "/org/bluez/hci0/dev_54_6C_0E_B7_20_04"

/// CC2650 Motion Service (MPU-9250) UUIDs
#define MOTION_DATA_UUID "f000aa81-0451-4000-b000-000000000000"
#define MOTION_CONF_UUID "f000aa82-0451-4000-b000-000000000000"

static GMainLoop* loop = NULL;
static GDBusProxy* dev_proxy = NULL;
static GDBusProxy* data_proxy = NULL;
static GDBusProxy* conf_proxy = NULL;
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
/// Waits until BlueZ reports ServicesResolved for the device, iterating the main
/// context while it waits.
///
/// Iterating matters: a plain sleep loop would not process the D-Bus traffic that
/// sets the property, so it would never become true.
///
/// @param dproxy      The device proxy to watch.
/// @param timeout_sec How long to wait, in seconds (polled every 250 ms).
/// @return TRUE when services were resolved, FALSE on timeout.
static gboolean wait_for_services_resolved(GDBusProxy* dproxy, int timeout_sec)
{
    GVariant* val = g_dbus_proxy_get_cached_property(dproxy, "ServicesResolved");
    if (val) {
        gboolean resolved = g_variant_get_boolean(val);
        g_variant_unref(val);
        if (resolved) {
            return TRUE;
        }
    }

    for (int i = 0; i < timeout_sec * 4; i++) {
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
/// Finds a GATT characteristic by UUID under a device path, through BlueZ's
/// ObjectManager.GetManagedObjects.
///
/// @param conn        The system-bus connection.
/// @param device_path The device whose subtree is searched.
/// @param target_uuid The characteristic UUID to match, case-insensitively.
/// @param error       Receives a GError on failure; may be NULL.
/// @return A proxy for the characteristic, or NULL when it was not found.
static GDBusProxy* find_char_by_uuid(GDBusConnection* conn,
                                     const char* device_path,
                                     const char* target_uuid,
                                     GError** error)
{
    GDBusProxy* om = g_dbus_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME, "/",
                                           OBJECT_MANAGER, NULL, error);
    if (!om) {
        return NULL;
    }

    GVariant* objects = g_dbus_proxy_call_sync(om, "GetManagedObjects", NULL,
                                               G_DBUS_CALL_FLAGS_NONE, 10000, NULL, error);
    g_object_unref(om);

    if (!objects) {
        return NULL;
    }

    GDBusProxy* result = NULL;
    GVariantIter* obj_iter;
    g_variant_get(objects, "(a{oa{sa{sv}}})", &obj_iter);

    const char* obj_path = NULL;
    GVariant* ifaces_var = NULL;
    while (g_variant_iter_next(obj_iter, "{&o@a{sa{sv}}}", &obj_path, &ifaces_var)) {
        if (!g_str_has_prefix(obj_path, device_path)) {
            g_variant_unref(ifaces_var);
            continue;
        }

        GVariantIter* iface_iter = NULL;
        const char* iface_name = NULL;
        GVariant* props_var = NULL;
        g_variant_get(ifaces_var, "a{sa{sv}}", &iface_iter);

        while (g_variant_iter_next(iface_iter, "{&s@a{sv}}", &iface_name, &props_var)) {
            if (g_strcmp0(iface_name, CHAR_INTERFACE) == 0) {
                GVariant* uuid_var =
                        g_variant_lookup_value(props_var, "UUID", G_VARIANT_TYPE_STRING);
                if (uuid_var) {
                    const char* uuid = g_variant_get_string(uuid_var, NULL);
                    if (g_ascii_strcasecmp(uuid, target_uuid) == 0) {
                        result = g_dbus_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                                       BLUEZ_BUS_NAME, obj_path, CHAR_INTERFACE,
                                                       NULL, NULL);
                        g_variant_unref(uuid_var);
                        g_variant_unref(props_var);
                        g_variant_unref(ifaces_var);
                        goto done;
                    }
                    g_variant_unref(uuid_var);
                }
            }
            g_variant_unref(props_var);
        }
        g_variant_iter_free(iface_iter);
        g_variant_unref(ifaces_var);
    }

done:
    g_variant_iter_free(obj_iter);
    g_variant_unref(objects);
    return result;
}

////////////////////////////////////////////////////////////////////////////////
/// Converts the sensor's 18-byte reading into gyroscope, accelerometer and
/// magnetometer triples: nine signed 16-bit little-endian values in the order
/// gyro XYZ, accel XYZ, mag XYZ, scaled to degrees/s, g and microtesla:
///
///   gyro  = value * (500.0 / 65536.0)  ->  degrees/s
///   accel = value * (8.0 / 32768.0)    ->  g
///   mag   = value * 1.0                ->  microtesla
///
/// @param data The raw characteristic value.
/// @param len Its length; anything shorter than 18 bytes leaves the outputs as
///            the caller left them.
/// @param gyro Receives the three gyroscope values, in degrees/s.
/// @param accel Receives the three accelerometer values, in g.
/// @param mag Receives the three magnetometer values, in microtesla.
static void convert_motion(const guchar* data, gsize len, double* gyro, double* accel, double* mag)
{
    if (len < 18) {
        return;
    }

    int16_t raw[9] = {0};
    for (int i = 0; i < 9; i++) {
        raw[i] = (int16_t)(data[2 * i] | ((int16_t)data[2 * i + 1] << 8));
    }

    for (int i = 0; i < 3; i++) {
        gyro[i] = (double)raw[i] * (500.0 / 65536.0);
        accel[i] = (double)raw[i + 3] * (8.0 / 32768.0);
        mag[i] = (double)raw[i + 6] * 1.0;
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Timer callback: reads the motion characteristic and prints the gyroscope,
/// accelerometer and magnetometer values.
///
/// @param user_data Unused.
/// @return G_SOURCE_CONTINUE, so the timer fires again.
static gboolean on_read_motion(gpointer user_data)
{
    (void)user_data;

    GError* error = NULL;
    GVariant* val = g_dbus_proxy_call_sync(data_proxy, "ReadValue", g_variant_new("(a{sv})", NULL),
                                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!val) {
        g_printerr("ReadValue failed: %s\n", error->message);
        g_error_free(error);
        return G_SOURCE_CONTINUE;
    }

    GVariant* bytes_variant = g_variant_get_child_value(val, 0);
    gsize len = 0;
    const guchar* bytes = g_variant_get_fixed_array(bytes_variant, &len, 1);
    if (len >= 18) {
        double gyro[3], accel[3], mag[3];
        convert_motion(bytes, len, gyro, accel, mag);

        // ANSI escape: move cursor 4 lines up to overwrite previous block
        printf("\033[4A");
        printf("--- MOTION DATA ---\n");
        printf("Accel (G):    X: %6.2f, Y: %6.2f, Z: %6.2f\n", accel[0], accel[1], accel[2]);
        printf("Gyro (deg/s): X: %6.1f, Y: %6.1f, Z: %6.1f\n", gyro[0], gyro[1], gyro[2]);
        printf("Mag (uT):     X: %6.1f, Y: %6.1f, Z: %6.1f\n", mag[0], mag[1], mag[2]);
    }

    g_variant_unref(bytes_variant);
    g_variant_unref(val);
    return G_SOURCE_CONTINUE;
}

////////////////////////////////////////////////////////////////////////////////
/// Shuts down in the right order: disables the sensors so they stop drawing
/// power, releases the proxies, and disconnects the device.
static void cleanup(void)
{
    printf("\nDisabling sensors to save battery...\n");

    if (conf_proxy) {
        guchar disable[] = {0x00, 0x00};
        GVariant* args = g_variant_new(
                "(@ay@a{sv})", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, disable, 2, 1),
                g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
        g_dbus_proxy_call_sync(conf_proxy, "WriteValue", args, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                               NULL);
        g_object_unref(conf_proxy);
        conf_proxy = NULL;
    }

    if (data_proxy) {
        g_object_unref(data_proxy);
        data_proxy = NULL;
    }

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

int main(void)
{
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    // 1. Connect to system D-Bus
    GError* error = NULL;
    connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        g_printerr("D-Bus connection failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    // 2. Create device proxy
    dev_proxy = g_dbus_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME,
                                      DEVICE_PATH, DEVICE_INTERFACE, NULL, &error);
    if (!dev_proxy) {
        g_printerr(
                "Device proxy failed.\n"
                "  Error: %s\n",
                error->message);
        g_error_free(error);
        g_object_unref(connection);
        return 1;
    }

    // 3. Connect
    printf("Connecting to SensorTag %s...\n", MAC);
    g_dbus_proxy_call_sync(dev_proxy, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);
    if (error) {
        g_printerr(
                "Connect failed: %s\n"
                "Make sure the SensorTag is nearby.\n",
                error->message);
        g_error_free(error);
        g_object_unref(dev_proxy);
        g_object_unref(connection);
        return 1;
    }
    printf("Connected. Waiting for services to resolve...\n");

    if (!wait_for_services_resolved(dev_proxy, 30)) {
        g_printerr("Services did not resolve within 30 s.\n");
        g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                               NULL);
        g_object_unref(dev_proxy);
        g_object_unref(connection);
        return 1;
    }
    printf("Services resolved.\n");

    // 4. Discover motion service characteristics
    printf("Discovering 9-axis motion sensors...\n");

    data_proxy = find_char_by_uuid(connection, DEVICE_PATH, MOTION_DATA_UUID, &error);
    if (!data_proxy) {
        g_printerr("Failed to find motion data characteristic: %s\n",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                               NULL);
        g_object_unref(dev_proxy);
        g_object_unref(connection);
        return 1;
    }
    printf("  Data characteristic found.\n");

    conf_proxy = find_char_by_uuid(connection, DEVICE_PATH, MOTION_CONF_UUID, &error);
    if (!conf_proxy) {
        g_printerr("Failed to find motion config characteristic: %s\n",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        g_object_unref(data_proxy);
        g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                               NULL);
        g_object_unref(dev_proxy);
        g_object_unref(connection);
        return 1;
    }
    printf("  Config characteristic found.\n");

    // 5. Enable all 9 axes (write 0x7F, 0x00 to config)
    printf("Enabling 9-Axis Motion Sensors...\n");
    {
        guchar enable[] = {0x7F, 0x00};
        GVariant* args = g_variant_new("(@ay@a{sv})",
                                       g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, enable, 2, 1),
                                       g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
        g_dbus_proxy_call_sync(conf_proxy, "WriteValue", args, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                               &error);
        if (error) {
            g_printerr("Failed to enable sensors: %s\n", error->message);
            g_clear_error(&error);
        }
    }

    // Stabilization delay (non-blocking to keep BLE alive)
    printf("Waiting for sensors to stabilize...\n");
    printf("--- MOTION DATA ---\n");
    printf("Accel (G):    X:   0.00, Y:   0.00, Z:   0.00\n");
    printf("Gyro (deg/s): X:   0.0,  Y:   0.0,  Z:   0.0\n");
    printf("Mag (uT):     X:   0.0,  Y:   0.0,  Z:   0.0\n");
    printf("\033[4A");
    g_timeout_add(2000, on_read_motion, NULL);

    // 7. Run event loop
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    // 8. Clean shutdown
    cleanup();
    return 0;
}
