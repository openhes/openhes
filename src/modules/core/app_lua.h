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
/// @brief HES gateway app (customer-specific protected app), driven by a Lua
/// script inside the core service module.
///
/// @details
/// Per ISO/IEC 18012-3 Annex A.1 and 15045-4-1 A.4, the app never touches
/// HES-CLME directly. It reaches service objects and the binding map only
/// through the two C functions this module exposes to Lua:
///
///   - hes.get(path): looks up a LOCAL service object by Lexicon path and
///     returns its current value (a string), or nil if no local object answers
///     that path.
///
///   - hes.put(path, value): sends a PUT straight onto the module's binding map /
///     bus, exactly like the direct app-to-binding-map arrow shown in 18012-3's
///     Figure A.1 (bypassing service objects).
///
/// The script (app.lua) is demo-sized: its hes_op ref_id==1 branch returns the
/// button level, and docs/demo_lua_control_light.md shows swapping that branch
/// for a toggle. Automation logic lives in Lua, so it needs no C changes.

#ifndef OPENHES_SRC_MODULES_CORE_APP_LUA_H
#define OPENHES_SRC_MODULES_CORE_APP_LUA_H

#include "bm/bm.h"

#include "common/service_object.h"

#include <stdint.h>

typedef struct app_lua app_lua_t;

////////////////////////////////////////////////////////////////////////////////
/// objs/n_objs: the module's local service objects (id, time, ...),
/// used to answer hes.get() calls without a bus round-trip.
///
/// @param script_path The path to the Lua script to load.
/// @param bus The module's event bus (for sending PUTs).
/// @param objs The module's local service objects.
/// @param n_objs The number of local service objects.
/// @return A pointer to the created app_lua_t instance, or NULL on failure.
app_lua_t* app_lua_create(const char* script_path,
                          hes_bus_t* bus,
                          service_object_t** objs,
                          int n_objs);

////////////////////////////////////////////////////////////////////////////////
/// appService ('ap'): asks the script's global `hes_op(ref_id, inputs)` to
/// perform a binding-map row's operation and return its result. This is how a
/// row with op="ap" is evaluated: the binding map stays the only route onto
/// HES-CLME, while the operation itself is performed by the protected app
/// (ISO/IEC 18012-3 Table 26 / A.2.2).
///
/// The built-in operators take at most two operands, but the standard does not
/// bound an appService, so every input of the row is passed. Each operand
/// becomes a Lua table entry with:
///
///   { di = <deviceIndex>, path = "<source object>", value = <number> }
///
/// letting the script identify its inputs by deviceIndex or by Lexicon path and
/// implement arbitrary logic (AND/OR, thresholds, state).
///
/// @param app The app_lua_t instance.
/// @param ref_id The binding-map row's refId ('ri') selecting the operation.
/// @param operands The row's conditioned inputs, in table order.
/// @param n_operands How many entries operands holds.
/// @param out Receives the script's numeric result.
/// @return 0 on success, -1 when the script defines no hes_op or it errored.
int app_lua_call_op(app_lua_t* app,
                    uint32_t ref_id,
                    const bm_operand_t* operands,
                    int n_operands,
                    double* out);

////////////////////////////////////////////////////////////////////////////////
/// Calls the script's global on_tick(), if defined. Safe to call every
/// main-loop iteration.
///
/// @param app The app_lua_t instance.
void app_lua_tick(app_lua_t* app);

////////////////////////////////////////////////////////////////////////////////
/// Destroys the app_lua_t instance, closing the Lua state and freeing memory.
///
/// @param app The app_lua_t instance to destroy.
void app_lua_destroy(app_lua_t* app);

#endif  // #ifndef OPENHES_SRC_MODULES_CORE_APP_LUA_H
