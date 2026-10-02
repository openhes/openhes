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
/// @brief Implementation of the Lua app bridge (app_lua.h): the Lua state, the
/// hes.* API, and the entry point the binding map calls for an appService row.
///
/// @details
/// The bridge hands the script a small, deliberate API and nothing else -- see
/// app_lua.h for the standards background:
///
///   - l_hes_get(): hes.get(path) answers from the module's local service
///     objects, with no bus round-trip.
///
///   - hes.put(): asks the module's own binding map for the PUT rather than
///     putting anything on the bus itself.
///
///   - app_lua_call_op(): the appService entry point, called by the binding map
///     for a row whose operation is 'ap', and app_lua_tick() for the script's own
///     periodic work.
///
/// A single file-static g_app keeps the Lua C-function signatures simple: this
/// POC hosts exactly one app per module (see the note at g_app for what to do if
/// that changes).

#include "app_lua.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct app_lua {
    lua_State* L;
    hes_bus_t* bus;
    service_object_t** objs;
    int n_objs;
};

/// We only ever have one app_lua_t in this POC (one app per module);
/// a raw global keeps the Lua C-function signatures simple. If you
/// later host multiple Lua apps in one process, move this into Lua's
/// registry (luaL_ref) instead.
static app_lua_t* g_app = NULL;

////////////////////////////////////////////////////////////////////////////////
/// hes.get(path) -> string value | nil
static int l_hes_get(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    if (!g_app) {
        lua_pushnil(L);
        return 1;
    }

    for (int i = 0; i < g_app->n_objs; i++) {
        service_object_t* so = g_app->objs[i];
        if (strcmp(so->path, path) == 0 && so->on_get) {
            hes_clme_msg_t out = {0};
            hes_msg_set_path(&out, path);
            so->on_get(so, &out);
            lua_pushstring(L, out.payload);
            return 1;
        }
    }

    // Not a local object. A fuller implementation would issue a GET on the bus
    // and block (with timeout) for the matching EVENT/response -- left as an
    // extension point since the app script does nothing yet.
    lua_pushnil(L);
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
/// hes.put(path, value) -> true|false
/// Sends the PUT directly onto the module's binding map / bus, matching
/// the direct app-to-binding-map path in 18012-3 Figure A.1.
static int l_hes_put(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
    if (!g_app) {
        lua_pushboolean(L, 0);
        return 1;
    }

    hes_clme_msg_t msg = {0};
    msg.verb = HES_VERB_PUT;
    hes_msg_set_path(&msg, path);
    hes_msg_set_payload_str(&msg, value);

    int rv = hes_bus_send(g_app->bus, &msg);
    lua_pushboolean(L, rv == 0);
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int app_lua_call_op(app_lua_t* app,
                    uint32_t ref_id,
                    const bm_operand_t* operands,
                    int n_operands,
                    double* out)
{
    if (!app || !app->L) {
        return -1;
    }

    lua_getglobal(app->L, "hes_op");
    if (!lua_isfunction(app->L, -1)) {
        lua_pop(app->L, 1);
        return -1;  // script defines no appService handler
    }

    lua_pushinteger(app->L, (lua_Integer)ref_id);

    // Build the operand table: one { di, path, value } entry per input.
    lua_createtable(app->L, n_operands, 0);
    for (int i = 0; i < n_operands; i++) {
        lua_createtable(app->L, 0, 3);
        lua_pushinteger(app->L, (lua_Integer)operands[i].device_index);
        lua_setfield(app->L, -2, "di");
        lua_pushstring(app->L, operands[i].source_object);
        lua_setfield(app->L, -2, "path");
        lua_pushnumber(app->L, operands[i].value);
        lua_setfield(app->L, -2, "value");
        lua_rawseti(app->L, -2, i + 1);
    }

    // Contained so a bad script cannot take down the binding-map processor.
    if (lua_pcall(app->L, 2, 1, 0) != LUA_OK) {
        fprintf(stderr, "app_lua: hes_op() error: %s\n", lua_tostring(app->L, -1));
        lua_pop(app->L, 1);
        return -1;
    }

    if (out) {
        *out = lua_tonumber(app->L, -1);
    }
    lua_pop(app->L, 1);
    return 0;
}

static const luaL_Reg hes_lib[] = {{"get", l_hes_get}, {"put", l_hes_put}, {NULL, NULL}};
app_lua_t* app_lua_create(const char* script_path,
                          hes_bus_t* bus,
                          service_object_t** objs,
                          int n_objs)
{
    app_lua_t* app = calloc(1, sizeof(*app));
    app->bus = bus;
    app->objs = objs;
    app->n_objs = n_objs;
    app->L = luaL_newstate();
    luaL_openlibs(app->L);

    // Register the "hes" table with get/put.
    luaL_newlib(app->L, hes_lib);
    lua_setglobal(app->L, "hes");

    g_app = app;  // see note on the global above

    if (luaL_dofile(app->L, script_path) != LUA_OK) {
        fprintf(stderr, "app_lua: error running %s: %s\n", script_path, lua_tostring(app->L, -1));
        lua_pop(app->L, 1);
    } else {
        fprintf(stderr, "app_lua: loaded %s\n", script_path);
    }

    return app;
}

void app_lua_tick(app_lua_t* app)
{
    if (!app || !app->L) {
        return;
    }
    lua_getglobal(app->L, "on_tick");
    if (lua_isfunction(app->L, -1)) {
        if (lua_pcall(app->L, 0, 0, 0) != LUA_OK) {
            fprintf(stderr, "app_lua: on_tick() error: %s\n", lua_tostring(app->L, -1));
            lua_pop(app->L, 1);
        }
    } else {
        lua_pop(app->L, 1);
    }
}

void app_lua_destroy(app_lua_t* app)
{
    if (!app) {
        return;
    }

    if (app->L) {
        lua_close(app->L);
    }

    if (g_app == app) {
        g_app = NULL;
    }

    free(app);
}
