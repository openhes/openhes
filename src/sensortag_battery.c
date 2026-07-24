#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <signal.h>

// =========================================================================
// CC2650 SensorTag — Battery Level reader via BlueZ D-Bus API
//
// Connects, discovers the Battery Level characteristic by UUID, reads
// the value once, and exits.
// =========================================================================

#define BLUEZ_BUS_NAME    "org.bluez"
#define DEVICE_INTERFACE   "org.bluez.Device1"
#define CHAR_INTERFACE     "org.bluez.GattCharacteristic1"
#define OBJECT_MANAGER     "org.freedesktop.DBus.ObjectManager"
#define ADAPTER_PATH       "/org/bluez/hci0"

#define MAC "54:6C:0E:B7:20:04"
#define DEVICE_PATH "/org/bluez/hci0/dev_54_6C_0E_B7_20_04"

// Standard Bluetooth Battery Service – Battery Level characteristic
#define BATTERY_CHAR_UUID "00002a19-0000-1000-8000-00805f9b34fb"

static GDBusProxy      *dev_proxy  = NULL;
static GDBusConnection *connection = NULL;

// -------------------------------------------------------------------------
// Wait until BlueZ reports that services have been resolved for this device
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
int
main(void)
{
	GError *error = NULL;

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

	// ---- 4. Discover battery level characteristic by UUID ----
	GDBusProxy *batt_proxy = find_char_by_uuid(connection,
	    DEVICE_PATH, BATTERY_CHAR_UUID, &error);
	if (!batt_proxy) {
		g_printerr("Failed to find Battery Level characteristic: %s\n",
		    error ? error->message : "unknown");
		g_clear_error(&error);
		g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
		g_object_unref(dev_proxy);
		g_object_unref(connection);
		return 1;
	}

	// ---- 5. Read battery level ----
	GVariant *val = g_dbus_proxy_call_sync(batt_proxy,
	    "ReadValue",
	    g_variant_new("(a{sv})", NULL),
	    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
	if (!val) {
		g_printerr("ReadValue failed: %s\n", error->message);
		g_error_free(error);
		g_object_unref(batt_proxy);
		g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
		g_object_unref(dev_proxy);
		g_object_unref(connection);
		return 1;
	}

	GVariant *bytes_var = g_variant_get_child_value(val, 0);
	gsize len;
	const guchar *bytes = g_variant_get_fixed_array(bytes_var, &len, 1);

	int batt_level = -1;
	if (len >= 1)
		batt_level = (int)bytes[0];

	g_variant_unref(bytes_var);
	g_variant_unref(val);
	g_object_unref(batt_proxy);

	// ---- 6. Print result ----
	printf("\n--- BATTERY STATUS ---\n");
	if (batt_level >= 0) {
		printf("Battery Capacity: %d%%\n", batt_level);

		if (batt_level < 60) {
			printf("[Warning] Battery is too low to drive the "
			       "high-current Infrared Thermopile sensor.\n");
			printf("Replace the CR2032 battery to unlock the "
			       "temperature script.\n");
		} else {
			printf("[Notice] Battery capacity reports fine. "
			       "The registry loop might need an altered "
			       "warm-up delay.\n");
		}
	} else {
		printf("No battery data received.\n");
	}

	// ---- 7. Disconnect and exit ----
	g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL,
	    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
	g_object_unref(dev_proxy);
	g_object_unref(connection);

	return 0;
}
