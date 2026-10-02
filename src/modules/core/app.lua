--[[
@file
@brief HES gateway app (customer-specific protected app): the Lua script the core
service module runs.

@details
The app has no bus access of its own: per ISO/IEC 18012-3 Annex A.1 it
reaches HES-CLME only through the binding map. It has three entry points,
all provided by src/modules/core/app_lua.c:

  hes.get(path)    -> read a LOCAL service object (app -> service objects)
  hes.put(path, v) -> push a value through the binding map (app -> map)
  hes_op(...)      -> perform a binding-map operation (op="ap", appService),
                      receiving every input of that row

See src/bm/README.md, "Routing data through the Lua app", for the XML.

hes_op is optional: a map that never uses op="ap" simply never calls it.
]]

print("[lua-app] HES gateway app (customer-specific protected app) started")

local ticks = 0

function on_tick()
    ticks = ticks + 1

    -- Example: read the identification service's current value
    -- (answered locally, no bus round-trip needed).
    -- if ticks == 1 then
    --     local id_info = hes.get("/lx/ob/so/id/co/st/cv")
    --     print("[lua-app] identification service: " .. tostring(id_info))
    -- end

    -- Example: push a value straight through the binding map, bypassing
    -- service objects entirely (the direct app->binding-map arrow).
    -- if ticks == 10 then
    --     hes.put("/lx/ob/uo/li/ll/da/cv", "1")
    -- end
end

-- ---------------------------------------------------------------------------
-- appService: a binding-map row with op="ap" delegates its OPERATION to this
-- function. `ref_id` is the row's refId ('ri'), so one script can serve
-- several rows.
--
-- `inputs` is an array holding every input of the row, in table order:
--
--     inputs[1] = { di = 6, path = "/lx/ob/uo/ui/ud/da/cv", value = 1 }
--
-- Identify inputs by `di` or by `path`, then return the operation's numeric
-- result; the binding map PUTs it to that row's destination (dl/do).
-- Returning a value is not a bus write -- all routing stays in the map.
--
-- The built-in operators take at most two operands, but the standard does not
-- bound an appService, so use as many inputs as the row declares.
-- ---------------------------------------------------------------------------
function hes_op(ref_id, inputs)
    if ref_id == 1 then
        -- One input: the button decides the light.
        local pressed = inputs[1] and inputs[1].value or 0
        return pressed ~= 0 and 1 or 0
    end

    if ref_id == 2 then
        -- Three inputs: light on only when the button is pressed AND motion
        -- is detected AND it is dark. AND/OR logic like this is exactly what
        -- the appService is for.
        local pressed, motion, dark = false, false, false
        for _, o in ipairs(inputs) do
            if o.di == 6 then pressed = o.value ~= 0 end
            if o.di == 7 then motion = o.value ~= 0 end
            if o.di == 8 then dark = o.value > 80 end
        end
        return (pressed and motion and dark) and 1 or 0
    end

    return 0
end
