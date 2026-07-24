#include <stdio.h>
#include <glib.h>
#include <gio/gio.h>
#include <signal.h>

// =========================================================================
// CC2650 SensorTag — BLE button reader via BlueZ D-Bus API
//
// Handles the full lifecycle: connect -> pair -> discover -> notify -> cleanup
// No dependency on bluetoothctl or external pairing.
// =========================================================================

#define BLUEZ_BUS_NAME   "org.bluez"
#define DEVICE_INTERFACE "org.bluez.Device1"
#define CHAR_INTERFACE   "org.bluez.GattCharacteristic1"
#define ADAPTER_PATH     "/org/bluez/hci0"

#define MAC "54:6C:0E:B7:20:04"
#define DEVICE_PATH "/org/bluez/hci0/dev_54_6C_0E_B7_20_04"

// Known characteristic for TI SensorTag button notifications
#define BUTTON_CHAR_PATH DEVICE_PATH "/service004a/char004b"

static GMainLoop      *loop      = NULL;
static GDBusProxy     *char_proxy = NULL;
static GDBusProxy     *dev_proxy  = NULL;
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
// Called every time the characteristic's Properties change; filters for
// the "Value" property which carries the button state.
// -------------------------------------------------------------------------
static void
on_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
    GStrv invalidated_properties, gpointer user_data)
{
	(void) proxy;
	(void) invalidated_properties;
	(void) user_data;

	GVariant *value_variant =
	    g_variant_lookup_value(changed_properties, "Value", NULL);
	if (!value_variant)
		return;

	GVariantIter *iter;
	g_variant_get(value_variant, "ay", &iter);

	guchar state_byte = 0;
	if (g_variant_iter_next(iter, "y", &state_byte)) {
		if (state_byte == 0) {
			printf("-> [RELEASED] All buttons let go.\n");
		} else {
			printf("-> [PRESSED] ");
			if (state_byte & 0x01)
				printf("LEFT Button ");
			if (state_byte & 0x02)
				printf("RIGHT Button ");
			if (state_byte & 0x04)
				printf("REED Switch ");
			printf("\n");
		}
	}
	fflush(stdout);

	g_variant_iter_free(iter);
	g_variant_unref(value_variant);
}

// -------------------------------------------------------------------------
// Wait until BlueZ reports that services have been resolved for this device
//
// Must iterate the main context while waiting so D-Bus property-change
// signals actually arrive and update the proxy cache.  A plain sleep
// loop would block message processing and `ServicesResolved` would never
// flip from FALSE to TRUE.
// -------------------------------------------------------------------------
static gboolean
wait_for_services_resolved(GDBusProxy *dproxy, int timeout_sec)
{
	// Fast path – already resolved
	GVariant *val = g_dbus_proxy_get_cached_property(dproxy, "ServicesResolved");
	if (val) {
		gboolean resolved = g_variant_get_boolean(val);
		g_variant_unref(val);
		if (resolved)
			return TRUE;
	}

	// Poll – iterate the main context so D-Bus messages get processed
	for (int i = 0; i < timeout_sec * 4; i++) {
		// Process any pending D-Bus events so the property cache updates
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
// Clean shutdown -- stop notifications and disconnect BLE
// -------------------------------------------------------------------------
static void
cleanup(void)
{
	printf("\nCleaning up...\n");

	// Stop notifications
	if (char_proxy) {
		g_dbus_proxy_call_sync(char_proxy, "StopNotify", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
		g_object_unref(char_proxy);
		char_proxy = NULL;
	}

	// Disconnect from the device
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
		           "  Try: bluetoothctl scan on  (then check device path)\n"
		           "  Error: %s\n", error->message);
		g_error_free(error);
		g_object_unref(connection);
		return 1;
	}

	// ---- 3. Connect (pairs automatically if needed) ----
	printf("Connecting to SensorTag %s...\n", MAC);
	g_dbus_proxy_call_sync(dev_proxy, "Connect", NULL,
	    G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);
	if (error) {
		g_printerr("Connect failed: %s\n"
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
		g_dbus_proxy_call_sync(dev_proxy, "Disconnect", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
		g_object_unref(dev_proxy);
		g_object_unref(connection);
		return 1;
	}
	printf("Services resolved.\n");

	// ---- 4. Create characteristic proxy and start notifications ----
	char_proxy = g_dbus_proxy_new_sync(connection,
	    G_DBUS_PROXY_FLAGS_NONE, NULL,
	    BLUEZ_BUS_NAME, BUTTON_CHAR_PATH, CHAR_INTERFACE,
	    NULL, &error);
	if (!char_proxy) {
		g_printerr("Characteristic proxy failed.\n"
		           "  Path: %s\n"
		           "  Error: %s\n",
		    BUTTON_CHAR_PATH, error->message);
		g_error_free(error);
		// Don't return -- still try to clean up
	} else {
		// Connect signal
		g_signal_connect(char_proxy, "g-properties-changed",
		    G_CALLBACK(on_properties_changed), NULL);

		// StartNotify
		printf("Subscribing to button notifications...\n");
		GVariant *reply = g_dbus_proxy_call_sync(char_proxy,
		    "StartNotify", NULL,
		    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
		if (!reply) {
			g_printerr("StartNotify failed: %s\n",
			    error ? error->message : "unknown");
			if (error)
				g_error_free(error);
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
