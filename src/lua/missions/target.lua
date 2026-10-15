-- lualoader, R"EOF(--
-- Capture before a chosen instruction or immediately after restoring a state.
local m = require 'support'
m.begin('target,savestate,buttons,slot,frames', function()
    local save = m.integer('savestate', 0, 0, 1) == 1
    local slot = m.integer('slot', 1, 1, 2)
    local names, buttons, seen = m.argument('buttons', ''), {}, {}
    local rebuilt = {}
    for name in names:gmatch('[^,]+') do
        local button = assert(PCSX.CONSTS.PAD.BUTTON[name], 'unknown button: ' .. name)
        assert(not seen[name], 'duplicate button: ' .. name)
        buttons[#buttons + 1], rebuilt[#rebuilt + 1], seen[name] = button, name, true
    end
    assert(table.concat(rebuilt, ',') == names, 'buttons must be comma separated names')
    local bounded = m.argument('frames') ~= nil or #buttons > 0
    local maximum = bounded and m.integer('frames', 36000, 1, 36000)
    local pad, observed, exhausted = PCSX.SIO0.slots[slot].pads[1], 0, false
    local function release()
        for _, button in ipairs(buttons) do pad.clearOverride(button) end
    end
    m.atTarget(function()
        if exhausted then return end
        release()
        m.capture(save)
        if bounded or names ~= '' then
            m.report('target.json', {schema = 'psx.runtime-target/v1', vsyncs = observed,
                slot = slot, buttons = names, registers = m.registers()})
        end
        m.finish()
    end, function()
        m.requireIdleCapture()
        for _, button in ipairs(buttons) do pad.setOverride(button) end
        if not bounded then return end
        m.event('GPU::Vsync', function()
            observed = observed + 1
            if observed ~= maximum then return end
            exhausted = true
            PCSX.pauseEmulator()
            release()
            PCSX.nextPause(m.guard(function()
                error('target not observed within ' .. maximum .. ' guest Vsyncs')
            end))
        end)
    end)
end)
-- )EOF"
