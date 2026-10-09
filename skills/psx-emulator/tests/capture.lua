-- Exercise the real installed audio recorder; callback signal checks use guest fixtures.
local m, bit = require 'support', require 'bit'
m.begin('', function()
    PCSX.pauseEmulator()
    local h = assert(PCSX.Audio, 'native audio capture binding required')
    for _, args in ipairs({{0,1,1}, {2646001,1,1}, {1,0,1}, {1,65537,1},
        {1,1,0}, {1,1,65537}, {1.5,1,1}}) do
        assert(not pcall(h.begin, unpack(args)), 'invalid audio limits accepted')
    end
    assert(not pcall(h.stop) and not pcall(h.samples, 0, 0), 'empty drain accepted')
    PCSX.resumeEmulator()
    assert(not pcall(h.begin, 44100, 1024, 1024), 'running begin accepted')
    PCSX.pauseEmulator()
    h.begin(44100, 1024, 1024)
    assert(not pcall(h.begin, 44100, 1024, 1024) and not pcall(h.clear), 'active evidence replaced')
    assert(not pcall(m.requireIdleCapture), 'host edit guard accepted active audio')
    assert(not pcall(h.block, 0) and not pcall(h.event, 0) and not pcall(h.samples, 0, 0),
        'active drain accepted')
    h.stop()
    local frozen = h.status()
    assert(frozen.state == 2 and frozen.failures == 0)
    assert(h.samples(0, 0) == '' and not pcall(h.samples, frozen.frames + 1, 1))
    assert(not pcall(h.block, frozen.blocks) and not pcall(h.event, frozen.events))
    assert(not pcall(h.begin, 44100, 1024, 1024), 'frozen evidence replaced without clear')
    PCSX.resumeEmulator()
    assert(not pcall(h.clear) and not pcall(h.samples, 0, 0), 'running drain accepted')
    PCSX.pauseEmulator(); h.clear()
    assert(frozen.state == 2 and h.status().state == 0, 'native status is borrowed')
    m.write('seed.pbuf', m.stateBytes())
    local failures = {}
    local settings = assert(PCSX.settings.spu)
    for _, name in ipairs({'Mute', 'Mono', 'Streaming', 'UseNullSync', 'IRQWait', 'DBufIRQ',
        'Volume', 'Interp', 'Reverb', 'Backend', 'Device'}) do
        local original, changed = settings[name], nil
        assert(original ~= nil, 'missing setting ' .. name)
        if type(original) == 'boolean' then changed = not original end
        if type(original) == 'number' then changed = original == 0 and 1 or 0 end
        if type(original) == 'string' then changed = original .. '-capture-check' end
        h.begin(44100, 1024, 1024)
        settings[name] = changed
        local observed = h.status()
        settings[name] = original
        h.stop()
        assert(bit.band(observed.failures, 64) ~= 0 and bit.band(h.status().failures, 64) ~= 0,
            name .. ' setting discontinuity concealed or cleared')
        failures[#failures + 1] = {operation = 'setting-' .. name, failures = observed.failures}
        h.clear()
    end
    local operations = {
        {'save', function() return pcall(PCSX.createSaveStateChecked) end},
        {'restore', function()
            local file = Support.File.open('seed.pbuf')
            local accepted = pcall(PCSX.loadSaveStateChecked, file); file:close(); return accepted
        end},
        {'soft-reset', PCSX.softResetEmulator},
        {'hard-reset', PCSX.hardResetEmulator},
    }
    for _, operation in ipairs(operations) do
        h.begin(44100, 1024, 1024)
        local checked = operation[1] == 'save' or operation[1] == 'restore'
        if checked then
            assert(not operation[2](), operation[1] .. ' accepted an active recorder')
            assert(h.status().state == 1 and h.status().failures == 0,
                operation[1] .. ' refusal mutated active recorder')
        else operation[2]() end
        PCSX.pauseEmulator(); h.stop()
        local status = h.status()
        if checked then assert(status.failures == 0, operation[1] .. ' refusal invalidated evidence')
        else assert(bit.band(status.failures, 64) ~= 0, operation[1] .. ' discontinuity concealed') end
        failures[#failures + 1] = {operation = operation[1], failures = status.failures,
            frames = status.frames, blocks = status.blocks, events = status.events}
        h.clear()
    end
    m.report('checks.json', {schema = 'psx.skill-checks/v1', passed = 9,
        checks = {'budgets', 'empty/running lifecycle', 'active ownership and drain',
            'frozen ownership and range', 'settings discontinuity', 'active checked save refusal', 'active checked restore refusal',
            'soft-reset discontinuity', 'hard-reset discontinuity'}, failures = failures})
    m.finish()
end)
