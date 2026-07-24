#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <signal.h>

// =========================================================================
// CC2650 SensorTag — Digital Microphone (SPH0641LU4H) reader via BlueZ
//
// Discovers the audio service characteristics by UUID, enables the
// microphone, and displays a real-time sound level meter.
// =========================================================================

#define BLUEZ_BUS_NAME    "org.bluez"
#define DEVICE_INTERFACE   "org.bluez.Device1"
#define CHAR_INTERFACE     "org.bluez.GattCharacteristic1"
#define OBJECT_MANAGER     "org.freedesktop.DBus.ObjectManager"
#define ADAPTER_PATH       "/org/bluez/hci0"

#define MAC "54:6C:0E:B7:20:04"
#define DEVICE_PATH "/org/bluez/hci0/dev_54_6C_0E_B7_20_04"

// CC2650 Audio / Digital Microphone UUIDs
#define AUDIO_DATA_UUID "f000aa61-0451-4000-b000-000000000000"
#define AUDIO_CONF_UUID "f000aa62-0451-4000-b000-000000000000"

static GMainLoop      *loop       = NULL;
static GDBusProxy     *dev_proxy  = NULL;
static GDBusProxy     *data_proxy = NULL;
static GDBusProxy     *conf_proxy = NULL;
static GDBusConnection *connection = NULL;

// -------------------------------------------------------------------------
// Signal handler -- quit main loop so cleanup runs
// -------------------------------------------------------------------------
static void
handle_sigint(int sig)
{
	(void) sig;
	if (loop && g_main_loop_is_running(loop))
		g_main_loop_quit(loop);
}

// -------------------------------------------------------------------------
// Wait until BlueZ reports that services have been resolved
// -------------------------------------------------------------------------
static gboolean
wait_for_services_resolved(GDBusProxy *dproxy, int timeout_sec)
{
	GVariant *val = g_dbus_proxy_get_cached_property(dproxy, "ServicesResolved");
	if (val) {
		gboolean resolved = g_variant_get_boolean(val);
		g_variant_unref(val);
		if (resolved)
			return TRUE;
	}

	for (int i = 0; i < timeout_sec * 4; i++) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(250000);

		val = g_dbus_proxy_get_cached_property(dproxy, "ServicesResolved");
		if (val) {
			gboolean resolved = g_variant_get_boolean(val);
			g_variant_unref(val);
			if (resolved)
				return TRUE;
		}
	}
	return FALSE;
}

// -------------------------------------------------------------------------
// Use ObjectManager.GetManagedObjects to find a GATT characteristic with
// the given UUID under the given device path.
// -------------------------------------------------------------------------
static GDBusProxy *
find_char_by_uuid(GDBusConnection *conn,
    const char *device_path, const char *target_uuid, GError **error)
{
	GDBusProxy *om = g_dbus_proxy_new_sync(conn,
	    G_DBUS_PROXY_FLAGS_NONE, NULL,
	    BLUEZ_BUS_NAME, "/", OBJECT_MANAGER, NULL, error);
	if (!om)
		return NULL;

	GVariant *objects = g_dbus_proxy_call_sync(om,
	    "GetManagedObjects", NULL,
	    G_DBUS_CALL_FLAGS_NONE, 10000, NULL, error);
	g_object_unref(om);

	if (!objects)
		return NULL;

	GDBusProxy *result = NULL;
	GVariantIter *obj_iter;
	g_variant_get(objects, "a{oa{sa{sv}}}", &obj_iter);

	const char *obj_path;
	GVariant *ifaces_var;
	while (g_variant_iter_next(obj_iter,
	    "{&o@a{sa{sv}}}", &obj_path, &ifaces_var)) {
		if (!g_str_has_prefix(obj_path, device_path)) {
			g_variant_unref(ifaces_var);
			continue;
		}

		GVariantIter *iface_iter;
		const char *iface_name;
		GVariant *props_var;
		g_variant_get(ifaces_var, "a{sa{sv}}", &iface_iter);

		while (g_variant_iter_next(iface_iter,
		    "{&s@a{sv}}", &iface_name, &props_var)) {
			if (g_strcmp0(iface_name, CHAR_INTERFACE) == 0) {
				GVariant *uuid_var =
				    g_variant_lookup_value(props_var,
				        "UUID", G_VARIANT_TYPE_STRING);
				if (uuid_var) {
					const char *uuid =
					    g_variant_get_string(uuid_var, NULL);
					if (g_ascii_strcasecmp(uuid,
					    target_uuid) == 0) {
						result = g_dbus_proxy_new_sync(
						    conn,
						    G_DBUS_PROXY_FLAGS_NONE,
						    NULL,
						    BLUEZ_BUS_NAME,
						    obj_path,
						    CHAR_INTERFACE,
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

// -------------------------------------------------------------------------
// Calculate average amplitude from raw audio samples (unsigned 8-bit).
//
// For each sample the deviation from the zero-baseline (128) is summed,
// then divided by the sample count to get a relative sound level.
// -------------------------------------------------------------------------
static double
convert_audio_level(const guchar *data, gsize len)
{
	if (len == 0)
		return 0.0;

	unsigned long total = 0;
	for (gsize i = 0; i < len; i++) {
		int dev = (int)data[i] - 128;
		total += (dev < 0) ? (unsigned long)(-dev) : (unsigned long)dev;
	}
	return (double)total / (double)len;
}

// -------------------------------------------------------------------------
// Periodic timer callback -- read audio data and print meter
// -------------------------------------------------------------------------
static gboolean
on_read_audio(gpointer user_data)
{
	(void) user_data;

	GError *error = NULL;

	GVariant *val = g_dbus_proxy_call_sync(data_proxy,
	    "ReadValue",
	    g_variant_new("(a{sv})", NULL),
	    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
	if (!val) {
		g_printerr("ReadValue failed: %s\n", error->message);
		g_error_free(error);
		return G_SOURCE_CONTINUE;
	}

	GVariant *bytes_variant = g_variant_get_child_value(val, 0);
	gsize len;
	const guchar *bytes =
	    g_variant_get_fixed_array(bytes_variant, &len, 1);

	if (len > 0) {
		double level = convert_audio_level(bytes, len);

		// Clamp to 30 for the meter bar
		int bar_len = (level < 30.0) ? (int)level : 30;
		printf("Sound Intensity: [");
		for (int i = 0; i < bar_len; i++)
			putchar('#');
		for (int i = bar_len; i < 30; i++)
			putchar(' ');
		printf("] (%5.1f)   \r", level);
	} else {
		printf("Microphone warming up...                           \r");
	}
	fflush(stdout);

	g_variant_unref(bytes_variant);
	g_variant_unref(val);
	return G_SOURCE_CONTINUE;
}

// -------------------------------------------------------------------------
// Clean shutdown -- disable sensor, disconnect BLE
// -------------------------------------------------------------------------
static void
cleanup(void)
{
	printf("\nDisabling microphone to save battery...\n");

	if (conf_proxy) {
		guchar disable = 0x00;
		GVariant *args = g_variant_new("(ay@a{sv})",
		    g_variant_new_fixed_array(
		        G_VARIANT_TYPE_BYTE, &disable, 1, 1),
		    g_variant_new_array(
		        G_VARIANT_TYPE_VARDICT, NULL, 0));
		g_dbus_proxy_call_sync(conf_proxy, "WriteValue", args,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
		g_object_unref(conf_proxy);
		conf_proxy = NULL;
	}

	if (data_proxy) {
		g_object_unref(data_proxy);
		data_proxy = NULL;
	}

	if (dev_proxy) {
		g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
		g_object_unref(dev_proxy);
		dev_proxy = NULL;
	}

	if (connection)
		g_object_unref(connection);
	if (loop)
		g_main_loop_unref(loop);

	printf("Disconnected. Exiting.\n");
}

// -------------------------------------------------------------------------
int
main(void)
{
	GError *error = NULL;

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
	dev_proxy = g_dbus_proxy_new_sync(connection,
	    G_DBUS_PROXY_FLAGS_NONE, NULL,
	    BLUEZ_BUS_NAME, DEVICE_PATH, DEVICE_INTERFACE,
	    NULL, &error);
	if (!dev_proxy) {
		g_printerr("Device proxy failed. Is the MAC correct?\n"
		           "  Error: %s\n", error->message);
		g_error_free(error);
		g_object_unref(connection);
		return 1;
	}

	// ---- 3. Connect ----
	printf("Connecting to SensorTag %s...\n", MAC);
	g_dbus_proxy_call_sync(dev_proxy, "Connect", NULL,
	    G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);
	if (error) {
		g_printerr("Connect failed: %s\n"
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
		g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
		g_object_unref(dev_proxy);
		g_object_unref(connection);
		return 1;
	}
	printf("Services resolved.\n");

	// ---- 4. Discover audio service characteristics ----
	printf("Discovering digital microphone...\n");

	data_proxy = find_char_by_uuid(connection,
	    DEVICE_PATH, AUDIO_DATA_UUID, &error);
	if (!data_proxy) {
		g_printerr("Failed to find audio data characteristic: %s\n",
		    error ? error->message : "unknown");
		g_clear_error(&error);
		g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
		g_object_unref(dev_proxy);
		g_object_unref(connection);
		return 1;
	}
	printf("  Data characteristic found.\n");

	conf_proxy = find_char_by_uuid(connection,
	    DEVICE_PATH, AUDIO_CONF_UUID, &error);
	if (!conf_proxy) {
		g_printerr("Failed to find audio config characteristic: %s\n",
		    error ? error->message : "unknown");
		g_clear_error(&error);
		g_object_unref(data_proxy);
		g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
		g_object_unref(dev_proxy);
		g_object_unref(connection);
		return 1;
	}
	printf("  Config characteristic found.\n");

	// ---- 5. Enable microphone ----
	printf("Enabling Digital Microphone Hardware...\n");
	{
		guchar enable = 0x01;
		GVariant *args = g_variant_new("(ay@a{sv})",
		    g_variant_new_fixed_array(
		        G_VARIANT_TYPE_BYTE, &enable, 1, 1),
		    g_variant_new_array(
		        G_VARIANT_TYPE_VARDICT, NULL, 0));
		g_dbus_proxy_call_sync(conf_proxy, "WriteValue", args,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
		if (error) {
			g_printerr("Failed to enable microphone: %s\n",
			    error->message);
			g_clear_error(&error);
		}
	}

	// Wait for hardware to stabilize
	printf("Waiting for microphone to stabilize...\n");
	g_usleep(1500000);

	// ---- 6. Periodic reading (every 200 ms to catch sound spikes) ----
	printf("\nMonitoring Sound Activity (Ctrl+C to stop):\n");
	g_timeout_add(200, on_read_audio, NULL);

	// ---- 7. Run event loop ----
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);

	// ---- 8. Clean shutdown ----
	cleanup();
	return 0;
}
