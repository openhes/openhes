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
/// @brief BLE connection manager, BLUEZ backend: real BlueZ D-Bus presence and
/// the real button value, through GLib/GDBus (see hes_ble.h).
///
/// @details
/// Implements the same hes_ble.h manager API as the SIM backend, but
/// drives the state machines from real BlueZ over D-Bus (GLib/GDBus).
/// Consolidates the idioms proven in src/devices/ti_cc2650/tests/sensortag_*_test.c:
///   - Device1.Connect()              (auto-pairs / uses existing bond)
///   - wait for ServicesResolved
///   - ObjectManager.GetManagedObjects (resolve MAC -> device object path)
///   - PropertiesChanged("Connected") (link-loss detection)
///
/// Behaviour achieved:
///   - tag powers on  -> path resolved (scan if needed) -> Connect
///     -> ServicesResolved -> ONLINE (event) in a few seconds;
///   - tag powers off -> PropertiesChanged Connected=false -> link loss
///     -> OFFLINE event, reconnect scheduled;
///   - tag returns    -> next Connect attempt (short backoff, capped)
///     succeeds quickly because BlueZ remembers the bond.
///
/// PRESENCE layer: connect/reconnect/link-loss above, PLUS the real
/// SensorTag button: after services resolve it subscribes to the button
/// characteristic notifications (same known-good path suffix as
/// src/devices/ti_cc2650/tests/sensortag_button_test.c) and caches the
/// pressed state, which the module reads instead of its simulator. Other
/// sensor values (e.g. temperature) remain simulated -- see
/// src/devices/ti_cc2650/sensortag.h for the real GATT UUIDs and where to
/// swap them in.
///
/// Build: -DBLE_BACKEND=bluez (the CMake default; needs libglib2.0-dev).

#include "hes_ble.h"

#include <gio/gio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLUEZ_BUS_NAME "org.bluez"
#define DEVICE_INTERFACE "org.bluez.Device1"
#define ADAPTER_INTERFACE "org.bluez.Adapter1"
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define ADAPTER_PATH "/org/bluez/hci0"
#define CHAR_INTERFACE "org.bluez.GattCharacteristic1"

/// Known-good button characteristic path suffix, taken from the working
/// reader src/devices/ti_cc2650/tests/sensortag_button_test.c. BlueZ numbers
/// GATT objects by discovery order, so this can differ per SensorTag unit --
/// adjust if button notifications don't arrive on yours.
#define BTN_CHAR_SUFFIX "/service004a/char004b"

#define CONNECT_TIMEOUT_MS 8000
#define RESOLVE_TIMEOUT_MS 6000
#define SCAN_RETRY_S 3  ///< re-query managed objects this often
#define RETRY_BASE_S 1  ///< first reconnect attempt in ~1 s
#define RETRY_MAX_S 10  ///< backoff cap

typedef struct bluez_dev {
    uint32_t device_index;
    char mac[64];
    char path[256];    ///< BlueZ object path, e.g. /org/bluez/hci0/dev_...
    int connected;     ///< last known Connected property
    GDBusProxy* dev;   ///< org.bluez.Device1 proxy (when path known)
    GDBusProxy* btn;   ///< org.bluez.GattCharacteristic1 (button)
    gulong btn_sig;    ///< g-properties-changed signal id
    time_t next_try;   ///< when to (re)attempt connect / look up path
    int attempt;       ///< consecutive failures (backoff)
    int scanning;      ///< StartDiscovery issued
    int have_button;   ///< a real button notify value has been seen
    int button_state;  ///< 1 pressed, 0 released (any SensorTag key)
} bluez_dev_t;

static GDBusConnection* g_conn = NULL;
static guint g_sig_id = 0;
static bluez_dev_t g_dev[HES_BLE_MAX_DEVS];
static int g_n = 0;

////////////////////////////////////////////////////////////////////////////////
/// Finds the slot a deviceIndex is hosted in, so a D-Bus signal (which names a
/// device, not our index) can be traced back to the registry entry.
///
/// @param device_index The deviceIndex to look for.
/// @return The slot, or NULL when this manager does not host that device.
static bluez_dev_t* find_slot(uint32_t device_index)
{
    for (int i = 0; i < g_n; i++) {
        if (g_dev[i].device_index == device_index) {
            return &g_dev[i];
        }
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// GDBus handler for "PropertiesChanged" on org.bluez.Device1: records the
/// Connected property, which is how a link loss is noticed. BlueZ pushes this
/// signal; nothing here polls.
///
/// A signal names a D-Bus object path, never our deviceIndex, so the change is
/// attributed by matching that path against the managed devices -- an event for
/// somebody else's device is ignored. A PropertiesChanged that does not carry
/// Connected leaves the cached value as it was.
///
/// @param conn        The connection the signal arrived on (unused).
/// @param sender      The well-known name of the sender (unused).
/// @param object_path The D-Bus path of the device whose properties changed.
/// @param iface       The interface the change came from (unused).
/// @param signal_name The signal's name (unused).
/// @param params      The signal's parameters, (sa{sv}as); child 1 is the map of
///                    changed properties.
/// @param user_data   The data the subscription was made with (unused).
static void on_device_props(GDBusConnection* conn,
                            const gchar* sender,
                            const gchar* object_path,
                            const gchar* iface,
                            const gchar* signal_name,
                            GVariant* params,
                            gpointer user_data)
{
    (void)conn;
    (void)sender;
    (void)iface;
    (void)signal_name;
    (void)user_data;

    if (!object_path) {
        return;
    }
    for (int i = 0; i < g_n; i++) {
        if (!g_dev[i].path[0] || strcmp(g_dev[i].path, object_path) != 0) {
            continue;
        }
        // params is (sa{sv}as): index 1 holds the changed properties
        GVariant* changed = g_variant_get_child_value(params, 1);
        GVariant* v = g_variant_lookup_value(changed, "Connected", NULL);
        if (v) {
            g_dev[i].connected = g_variant_get_boolean(v);
            g_variant_unref(v);
        }
        g_variant_unref(changed);
        return;
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Resolves a device's D-Bus object path from its MAC address.
///
/// BlueZ names the path after the MAC, but the path only exists once BlueZ has
/// seen the device, so it cannot be computed here: this asks the ObjectManager
/// for every managed object and matches the Device1 "Address" property,
/// case-insensitively.
///
/// @param mac The device's Bluetooth address, e.g. "54:6C:0E:B7:20:04".
/// @return A newly allocated path string, which the caller must g_free(), or
///         NULL when there is no bus connection or BlueZ does not know the
///         device yet -- the normal state while a tag is off.
static char* resolve_path(const char* mac)
{
    if (!g_conn) {
        return NULL;
    }

    GError* error = NULL;
    GDBusProxy* om = g_dbus_proxy_new_sync(g_conn, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME,
                                           "/", OBJECT_MANAGER, NULL, &error);
    if (!om) {
        g_printerr("hes_ble[bluez]: ObjectManager proxy failed: %s\n",
                   error ? error->message : "?");
        if (error) {
            g_error_free(error);
        }
        return NULL;
    }

    GVariant* objects = g_dbus_proxy_call_sync(om, "GetManagedObjects", NULL,
                                               G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
    g_object_unref(om);
    if (!objects) {
        if (error) {
            g_error_free(error);
        }
        return NULL;
    }

    char* result = NULL;
    GVariantIter* oi;
    g_variant_get(objects, "(a{oa{sa{sv}}})", &oi);
    const char* opath;
    GVariant* ifaces;
    while (g_variant_iter_next(oi, "{&o@a{sa{sv}}}", &opath, &ifaces)) {
        GVariantIter* ii;
        const char* iname;
        GVariant* props;
        g_variant_get(ifaces, "a{sa{sv}}", &ii);
        while (g_variant_iter_next(ii, "{&s@a{sv}}", &iname, &props)) {
            if (g_strcmp0(iname, DEVICE_INTERFACE) == 0) {
                GVariant* addr = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
                if (addr) {
                    const char* a = g_variant_get_string(addr, NULL);
                    if (g_ascii_strcasecmp(a, mac) == 0) {
                        result = g_strdup(opath);
                        g_variant_iter_free(ii);
                        g_variant_unref(addr);
                        g_variant_unref(props);
                        g_variant_unref(ifaces);
                        goto done;
                    }
                    g_variant_unref(addr);
                }
            }
            g_variant_unref(props);
        }
        g_variant_iter_free(ii);
        g_variant_unref(ifaces);
    }
done:
    g_variant_iter_free(oi);
    g_variant_unref(objects);
    return result;
}

////////////////////////////////////////////////////////////////////////////////
/// Asks bluez to start scanning, so a SensorTag that is not yet connected
/// advertises itself. Best effort: without a connection, or without an adapter
/// proxy, it quietly does nothing.
static void start_discovery(void)
{
    if (!g_conn) {
        return;
    }
    GError* error = NULL;
    GDBusProxy* ad = g_dbus_proxy_new_sync(g_conn, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME,
                                           ADAPTER_PATH, ADAPTER_INTERFACE, NULL, &error);
    if (!ad) {
        if (error) {
            g_error_free(error);
        }
        return;
    }
    g_dbus_proxy_call_sync(ad, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    g_object_unref(ad);
}

////////////////////////////////////////////////////////////////////////////////
/// Asks bluez to stop scanning, so the radio is not left scanning for the whole
/// run. Best effort in the same way as start_discovery().
static void stop_discovery(void)
{
    if (!g_conn) {
        return;
    }
    GDBusProxy* ad = g_dbus_proxy_new_sync(g_conn, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME,
                                           ADAPTER_PATH, ADAPTER_INTERFACE, NULL, NULL);
    if (!ad) {
        return;
    }
    g_dbus_proxy_call_sync(ad, "StopDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    g_object_unref(ad);
}

////////////////////////////////////////////////////////////////////////////////
/// Waits until BlueZ reports ServicesResolved on a device, i.e. its GATT
/// services are known and the device is ready to use.
///
/// Pumps the default GLib context while waiting: the proxies' cached properties
/// are only refreshed by dispatching D-Bus messages, so a plain sleep would
/// never see the flag turn true (this module has no GDBus thread of its own --
/// see the file header).
///
/// @param dproxy     The org.bluez.Device1 proxy to watch.
/// @param timeout_ms How long to wait; the flag is polled every 50 ms.
/// @return TRUE once ServicesResolved is true, FALSE when the timeout expires.
static gboolean wait_resolved(GDBusProxy* dproxy, int timeout_ms)
{
    int spins = timeout_ms / 50;
    for (int i = 0; i < spins; i++) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(50000);
        GVariant* val = g_dbus_proxy_get_cached_property(dproxy, "ServicesResolved");
        if (val) {
            gboolean r = g_variant_get_boolean(val);
            g_variant_unref(val);
            if (r) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
/// GDBus handler for "g-properties-changed" on the button characteristic: turns
/// a notify value into the cached pressed state.
///
/// The SensorTag's notify byte packs LEFT (0x01), RIGHT (0x02) and REED (0x04),
/// and any bit set counts as a key being pressed. That is a demo decision: the
/// three keys act as one button, which is all a one-button fixture needs.
///
/// @param proxy       The characteristic proxy (unused).
/// @param changed     The changed properties; "Value" carries the notify byte.
/// @param invalidated The invalidated property names (unused).
/// @param user_data   The bluez_dev_t the characteristic belongs to.
static void on_btn_value(GDBusProxy* proxy,
                         GVariant* changed,
                         GStrv invalidated,
                         gpointer user_data)
{
    (void)proxy;
    (void)invalidated;
    bluez_dev_t* d = user_data;
    GVariant* vv = g_variant_lookup_value(changed, "Value", NULL);
    if (!vv) {
        return;
    }
    GVariantIter* it;
    guchar b = 0;
    g_variant_get(vv, "ay", &it);
    if (g_variant_iter_next(it, "y", &b)) {
        d->have_button = 1;
        d->button_state = (b != 0);
    }
    g_variant_iter_free(it);
    g_variant_unref(vv);
}

////////////////////////////////////////////////////////////////////////////////
/// Drops the button characteristic's notify handler and releases its proxy.
/// Safe to call for a device that never subscribed.
///
/// @param d The device whose subscription is being dropped.
static void unsubscribe_button(bluez_dev_t* d)
{
    if (d->btn) {
        if (d->btn_sig) {
            g_signal_handler_disconnect(d->btn, d->btn_sig);
        }
        g_dbus_proxy_call_sync(d->btn, "StopNotify", NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                               NULL);
        g_object_unref(d->btn);
        d->btn = NULL;
    }
    d->have_button = 0;
    d->button_state = 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Subscribes to the button characteristic's notifications, so press/release
/// updates arrive as D-Bus signals (see on_btn_value()).
///
/// Needs the device's object path -- known once it has connected -- plus the
/// known-good characteristic path suffix (BTN_CHAR_SUFFIX: BlueZ numbers GATT
/// objects by discovery order, so it can differ per unit). A device that is
/// already subscribed is left alone. Failure is not fatal: presence keeps
/// working and the module's own button simulator stays in charge.
///
/// @param d The device to subscribe.
static void subscribe_button(bluez_dev_t* d)
{
    if (d->btn || !g_conn || d->path[0] == '\0') {
        return;
    }
    char cpath[320];
    snprintf(cpath, sizeof(cpath), "%s%s", d->path, BTN_CHAR_SUFFIX);
    GError* error = NULL;
    d->btn = g_dbus_proxy_new_sync(g_conn, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME, cpath,
                                   CHAR_INTERFACE, NULL, &error);
    if (!d->btn) {
        g_printerr("hes_ble[bluez]: di=%u button char not found (%s): %s\n", d->device_index, cpath,
                   error ? error->message : "?");
        if (error) {
            g_error_free(error);
        }
        return;  // presence keeps working; button stays simulated
    }
    d->btn_sig = g_signal_connect(d->btn, "g-properties-changed", G_CALLBACK(on_btn_value), d);
    GVariant* reply = g_dbus_proxy_call_sync(d->btn, "StartNotify", NULL, G_DBUS_CALL_FLAGS_NONE,
                                             5000, NULL, NULL);
    if (reply) {
        g_variant_unref(reply);
    }
    fprintf(stderr, "hes_ble[bluez]: di=%u button notify on (%s)\n", d->device_index, cpath);
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

void hes_ble_mgr_init(hes_ble_mgr_t* m)
{
    memset(m, 0, sizeof(*m));
    memset(g_dev, 0, sizeof(g_dev));
    g_n = 0;

    if (!g_conn) {
        GError* error = NULL;
        g_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (!g_conn) {
            g_printerr("hes_ble[bluez]: system bus unavailable: %s\n",
                       error ? error->message : "?");
            if (error) {
                g_error_free(error);
            }
            return;
        }
        g_sig_id = g_dbus_connection_signal_subscribe(
                g_conn, BLUEZ_BUS_NAME, DEVICE_INTERFACE, "PropertiesChanged", NULL, NULL,
                G_DBUS_SIGNAL_FLAGS_NONE, on_device_props, NULL, NULL);
    }
}

int hes_ble_mgr_add(hes_ble_mgr_t* m, uint32_t device_index, const char* address)
{
    if (find_slot(device_index)) {
        return 0;  // already managed
    }
    if (g_n >= HES_BLE_MAX_DEVS || m->n >= HES_BLE_MAX_DEVS) {
        return -1;
    }

    // mirror into the manager's public per-device array
    hes_ble_conn_t* c = &m->dev[m->n++];
    c->device_index = device_index;
    snprintf(c->address, sizeof(c->address), "%s", address ? address : "");
    c->state = HES_BLE_STATE_OFF;
    c->state_entered = time(NULL);

    bluez_dev_t* d = &g_dev[g_n++];
    memset(d, 0, sizeof(*d));
    d->device_index = device_index;
    snprintf(d->mac, sizeof(d->mac), "%s", address ? address : "");
    d->next_try = time(NULL);  // try immediately
    fprintf(stderr, "hes_ble[bluez]: managing di=%u mac=%s\n", device_index, d->mac);
    return g_n - 1;
}

int hes_ble_mgr_poll(hes_ble_mgr_t* m, hes_ble_event_t* events, int max)
{
    int nev = 0;
    time_t now = time(NULL);

    // let D-Bus signals/proxy updates arrive
    g_main_context_iteration(NULL, FALSE);

    for (int i = 0; i < m->n; i++) {
        bluez_dev_t* d = find_slot(m->dev[i].device_index);
        if (!d) {
            continue;
        }
        if (!g_conn) {
            continue;
        }

        // --- path unknown: (re)discover (scan if needed) ---
        if (d->path[0] == '\0' && now >= d->next_try) {
            char* p = resolve_path(d->mac);
            if (p) {
                snprintf(d->path, sizeof(d->path), "%s", p);
                g_free(p);
                d->scanning = 0;
                stop_discovery();
            } else if (!d->scanning) {
                start_discovery();
                d->scanning = 1;
            }
            d->next_try = now + SCAN_RETRY_S;
        }

        // --- ONLINE: watch for link loss via Connected property ---
        if (m->dev[i].state == HES_BLE_STATE_ONLINE) {
            // re-read cached Connected (catches signals we missed)
            if (d->dev) {
                GVariant* cc = g_dbus_proxy_get_cached_property(d->dev, "Connected");
                if (cc) {
                    d->connected = g_variant_get_boolean(cc);
                    g_variant_unref(cc);
                }
            }
            if (d->dev && !d->connected) {
                unsubscribe_button(d);
                m->dev[i].state = HES_BLE_STATE_OFF;
                m->dev[i].state_entered = now;
                d->attempt = 0;  // reset backoff for quick retry
                d->next_try = now + RETRY_BASE_S;
                if (nev < max) {
                    events[nev].device_index = d->device_index;
                    events[nev].is_online = 0;
                    nev++;
                }
                fprintf(stderr, "hes_ble[bluez]: di=%u link lost\n", d->device_index);
            }
            continue;
        }

        // --- OFF / CONNECTING: attempt a (re)connect ---
        if (d->path[0] == '\0' || now < d->next_try) {
            continue;
        }

        GError* error = NULL;
        if (!d->dev) {
            d->dev = g_dbus_proxy_new_sync(g_conn, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_BUS_NAME,
                                           d->path, DEVICE_INTERFACE, NULL, &error);
        }
        if (!d->dev) {
            if (error) {
                g_error_free(error);
            }
            goto connect_failed;
        }

        m->dev[i].state = HES_BLE_STATE_CONNECTING;
        m->dev[i].state_entered = now;
        fprintf(stderr, "hes_ble[bluez]: di=%u connecting (%s)...\n", d->device_index, d->path);

        if (g_dbus_proxy_call_sync(d->dev, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE,
                                   CONNECT_TIMEOUT_MS, NULL, &error) == NULL) {
            if (error) {
                g_error_free(error);
            }
            goto connect_failed;
        }

        // connected: wait until services are resolved, then ONLINE
        if (!wait_resolved(d->dev, RESOLVE_TIMEOUT_MS)) {
            // could not resolve yet; retry later but don't report online
            goto connect_failed;
        }

        GVariant* c = g_dbus_proxy_get_cached_property(d->dev, "Connected");
        d->connected = c ? g_variant_get_boolean(c) : TRUE;
        if (c) {
            g_variant_unref(c);
        }

        m->dev[i].state = HES_BLE_STATE_ONLINE;
        m->dev[i].state_entered = now;
        d->attempt = 0;
        d->next_try = 0;
        subscribe_button(d);
        if (nev < max) {
            events[nev].device_index = d->device_index;
            events[nev].is_online = 1;
            nev++;
        }
        fprintf(stderr, "hes_ble[bluez]: di=%u connected, services resolved\n", d->device_index);
        continue;

    connect_failed:
        // backoff, capped, then retry
        m->dev[i].state = HES_BLE_STATE_OFF;
        m->dev[i].state_entered = now;
        d->attempt++;
        int delay = RETRY_BASE_S << d->attempt;
        if (delay > RETRY_MAX_S) {
            delay = RETRY_MAX_S;
        }
        d->next_try = now + delay;
    }
    return nev;
}

void hes_ble_mgr_close(hes_ble_mgr_t* m)
{
    (void)m;
    for (int i = 0; i < g_n; i++) {
        unsubscribe_button(&g_dev[i]);
        if (g_dev[i].dev) {
            // best effort disconnect
            g_dbus_proxy_call_sync(g_dev[i].dev, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, 3000,
                                   NULL, NULL);
            g_object_unref(g_dev[i].dev);
            g_dev[i].dev = NULL;
        }
    }
    if (g_conn) {
        if (g_sig_id) {
            g_dbus_connection_signal_unsubscribe(g_conn, g_sig_id);
        }
        g_object_unref(g_conn);
        g_conn = NULL;
    }
    g_n = 0;
}

int hes_ble_mgr_button(const hes_ble_mgr_t* m, uint32_t device_index, int* pressed)
{
    (void)m;
    for (int i = 0; i < g_n; i++) {
        if (g_dev[i].device_index == device_index) {
            if (g_dev[i].have_button) {
                *pressed = g_dev[i].button_state;
                return 0;
            }
            return -1;
        }
    }
    return -1;
}
