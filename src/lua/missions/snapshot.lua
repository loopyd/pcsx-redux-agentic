-- lualoader, R"EOF(--
-- Decode through the emulator's schema and bundled lua-protobuf, never wire guesses.
local pb = require 'pb'
local M = {}
M.schema = PCSX.getSaveStateProtoSchema()
assert(require('protoc').new():load(M.schema))
pb.option('int64_as_string') -- Large integers retain the library's exact #decimal form.

local schemas = {}
function M.fields(kind)
    local schema = schemas[kind]
    if not schema then
        local canonical = pb.type(kind) or kind
        schema = schemas[canonical]
        if not schema then
            schema = {fields = {}, names = {}}
            for name, number, fieldType, _, label in pb.fields(canonical) do
                local field = {name = name, number = number, type = fieldType, label = label,
                    repeated = label == 'repeated' or label == 'packed'}
                schema.fields[#schema.fields + 1], schema.names[name] = field, field
            end
            table.sort(schema.fields, function(a, b) return a.number < b.number end)
            schemas[canonical] = schema
        end
        schemas[kind] = schema
    end
    return schema.fields, schema.names
end

function M.live()
    return M.decode(require('support').stateBytes())
end

-- Inspection of a supplied state is offline unless an execution boundary was requested.
function M.observe(callback)
    local m = require 'support'
    local path = m.input('state', false)
    if path and not m.argument('target') and not os.getenv('PSX_RUNTIME_ENTRY') then
        PCSX.pauseEmulator()
        callback(M.read(path), 'saved-state')
    else
        m.atTarget(function() callback(M.live(), 'live') end)
    end
end

local function decodeMessage(kind, bytes, strict)
    pb.option(strict and 'no_default_values' or 'auto_default_values')
    local ok, decoded = pcall(pb.decode, kind, bytes)
    pb.option('auto_default_values')
    assert(ok and decoded, 'invalid state protobuf: ' .. tostring(decoded))
    return decoded
end

local function version(decoded)
    assert(decoded.save_state_info and decoded.save_state_info.version == PCSX.getSaveStateInfo().version,
        'unsupported/missing state version')
    return decoded.save_state_info.version
end

function M.decode(bytes, strict)
    if strict then PCSX.validateSaveStateWire(bytes) end
    local decoded = decodeMessage('SaveState', bytes, strict)
    version(decoded)
    return decoded
end

function M.readBytes(path)
    local file = assert(io.open(path, 'rb'))
    local bytes = assert(file:read(67108865)); file:close()
    assert(#bytes <= 67108864, 'state exceeds 64 MiB')
    assert(bytes:sub(1, 2) ~= '\31\139', 'gzip GUI state: supply raw API protobuf')
    return bytes
end

function M.read(path, strict)
    return M.decode(M.readBytes(path), strict)
end

function M.decodeSelected(bytes, path, strict, missing)
    local selected = PCSX.selectSaveStateWire(bytes, path, strict or false)
    local stateVersion = version(decodeMessage('SaveState', selected.version_wire, strict))
    local value, kind = nil, selected.kind
    if selected.field_name then
        local _, fields = M.fields(selected.parent_type)
        kind = fields[selected.field_name].type
    end
    if selected.parent_present then
        local parent = decodeMessage(selected.parent_type, selected.wire, strict)
        if selected.field_name then value = parent[selected.field_name]
        else value = parent end
        if selected.indexed then value = value and value[1] end
    end
    assert(value ~= nil or missing, selected.indexed and 'state index out of range (indices start at 1)' or
        'field absent in state: ' .. path)
    return value, kind, selected.repeated, stateVersion
end

function M.readSelected(file, path, strict, missing)
    return M.decodeSelected(M.readBytes(file), path, strict, missing)
end

function M.select(value, path, missing)
    local kind, repeated = 'SaveState', false
    if path == '' then return value, kind, repeated end
    local consumed = ''
    for part in path:gmatch('[^.]+') do
        local name, index = part:match('^([%a_][%w_]*)%[(%d+)%]$')
        name = name or part:match('^([%a_][%w_]*)$')
        assert(name and not repeated, 'invalid query path: ' .. path)
        local _, fields = M.fields(kind)
        local field = fields[name]
        assert(field, 'unknown schema field: ' .. part)
        value, kind, repeated = value and value[name], field.type, field.repeated
        assert(value ~= nil or missing, 'field absent in state: ' .. part)
        if index then
            assert(repeated, 'index requires a repeated field')
            assert(tonumber(index) >= 1 and tonumber(index) <= 1000000, 'invalid state index')
            value = value and value[tonumber(index)]
            assert(value ~= nil or missing, 'state index out of range (indices start at 1)')
            repeated = false
        end
        consumed = consumed == '' and part or consumed .. '.' .. part
    end
    assert(consumed == path, 'invalid query path: ' .. path)
    return value, kind, repeated
end

function M.validate(value, mode)
    assert(mode == nil or mode == "execution", "unknown state validation mode")
    if mode == "execution" then
        assert(value.save_state_info and value.save_state_info.version == PCSX.getSaveStateInfo().version and
            value.save_state_info.version_string == PCSX.getSaveStateInfo().versionString,
            "execution requires the native checked state identity")
        assert(value.sio and type(value.sio.mcd1_data) == "string" and #value.sio.mcd1_data == 131072 and
            type(value.sio.mcd2_data) == "string" and #value.sio.mcd2_data == 131072,
            "execution requires complete saved memory cards")
    end
    for field, size in pairs({ram = 8388608, rom = 524288, exp1 = 8388608,
        hardware = 65536, sram = 2097152}) do
        assert(value.memory and type(value.memory[field]) == 'string' and
            #value.memory[field] == size, 'invalid saved memory region: ' .. field)
    end
    for field, count in pairs({gpr = 34, cp0 = 32, cp2d = 32, cp2c = 32}) do
        assert(value.registers and type(value.registers[field]) == 'table' and
            #value.registers[field] == count, 'invalid saved register bank: ' .. field)
    end
    assert(value.spu and #value.spu.channel == 24 and #value.spu.ram == 524288,
        'invalid saved SPU state')
end

function M.describe(value, kind, repeated, depth, budget)
    budget = budget or {remaining = 4096}
    budget.remaining = budget.remaining - 1
    assert(budget.remaining >= 0, 'query exceeds 4096 nodes; narrow the path/depth')
    if kind == 'bytes' and not repeated then
        local preview = value:sub(1, 32):gsub('.', function(c) return string.format('%02x', c:byte()) end)
        return {type = 'bytes', bytes = #value, preview_hex = preview, preview_truncated = #value > 32}
    end
    if type(value) ~= 'table' then return value end
    if repeated then
        if depth == 0 then return {type = kind, count = #value} end
        local items = require('support').array()
        for i, item in ipairs(value) do items[i] = M.describe(item, kind, false, depth - 1, budget) end
        return {type = kind, count = #value, items = items}
    end
    local fields = {}
    for _, field in ipairs(M.fields(kind)) do
        local item = {name = field.name, number = field.number, type = field.type, label = field.label,
            present = value[field.name] ~= nil}
        if depth > 0 and value[field.name] ~= nil then
            item.value = M.describe(value[field.name], field.type, field.repeated,
                depth - 1, budget)
        end
        fields[#fields + 1] = item
    end
    return {type = kind, fields = fields}
end

return M
-- )EOF"
