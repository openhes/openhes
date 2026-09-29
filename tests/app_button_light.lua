-- Toggle the light on press-and-release.
-- The binding map calls this whenever the button value changes, so the app
-- sees edge transitions: 1 (pressed), then 0 (released).

local light_on = false   -- our belief about the light; we assume it starts off
local pressed  = false   -- was the button down at the previous event?

function hes_op(ref_id, inputs)
    if ref_id == 1 then
        local btn = inputs[1] and inputs[1].value or 0
        local down = (btn ~= 0)

        if down then
            pressed = true               -- press: just remember it
        elseif pressed then              -- release after a press: flip
            pressed = false
            light_on = not light_on
        end

        return light_on and 1 or 0       -- the map PUTs this to the light
    end
    return 0
end
