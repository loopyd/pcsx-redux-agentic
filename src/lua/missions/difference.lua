-- lualoader, R"EOF(--
-- Exact bounded schema comparisons; no equality claim after omitted work.
local snapshot, m = require 'snapshot', require 'support'
local M = {}
local function integer(value, minimum, maximum)
    return type(value) == 'number' and value >= minimum and value <= maximum and value == math.floor(value)
end
function M.compare(before, after, kind, repeated, path, options)
    options = options or {}
    local report = {schema = 'psx.runtime-comparison/v2', path = path,
        scope = 'selected schema subtree', presence = 'encoded fields',
        equal = true, compared_nodes = 0, compared_bytes = 0,
        changed_bytes = 0, differences = m.array()}
    local limit, depth = options.limit or 1024, options.depth or 16
    local nodes, bytes = options.nodes or 100000, options.bytes or 67108864
    assert(integer(limit, 0, 65536), 'invalid comparison difference limit')
    assert(integer(depth, 0, 64), 'invalid comparison depth')
    assert(integer(nodes, 1, 100000), 'invalid comparison node limit')
    assert(integer(bytes, 0, 67108864), 'invalid comparison byte limit')
    local function add(entry)
        assert(#report.differences < limit, 'comparison difference limit exceeded; narrow the path or increase limit')
        report.equal = false
        report.differences[#report.differences + 1] = entry
    end
    local function summary(value, fieldType)
        if value == nil then return {present = false} end
        if fieldType == 'bytes' then return {present = true, bytes = #value} end
        if type(value) == 'table' then return {present = true, type = fieldType} end
        return {present = true, value = value}
    end
    local function preview(value, first, last)
        return value:sub(first, math.min(last, first + 31)):gsub('.', function(c)
            return string.format('%02x', c:byte())
        end)
    end
    local function compare(a, b, fieldType, array, fieldPath, remaining)
        report.compared_nodes = report.compared_nodes + 1
        assert(report.compared_nodes <= nodes, 'comparison node limit exceeded')
        if a == nil or b == nil then
            if a ~= b then add({path = fieldPath, kind = 'presence',
                before = summary(a, fieldType), after = summary(b, fieldType)}) end
            return
        end
        if not array and fieldType == 'bytes' then
            local count = math.max(#a, #b)
            report.compared_bytes = report.compared_bytes + count
            assert(report.compared_bytes <= bytes, 'comparison byte limit exceeded')
            assert(type(a) == 'string' and type(b) == 'string', 'incompatible decoded field: ' .. fieldPath)
            local found, refusal = PCSX.scanByteRanges(a, b, limit - #report.differences)
            if found == nil and refusal == 'limit' then
                error('comparison difference limit exceeded; narrow the path or increase limit')
            end
            assert(found and refusal == nil, 'invalid native byte scanner result')
            for _, range in ipairs(found.ranges) do
                local first, last = range.offset + 1, range.offset + range.length
                add({path = fieldPath, kind = 'bytes', offset = range.offset,
                    length = range.length, before_bytes = #a, after_bytes = #b,
                    before_hex = preview(a, first, last), after_hex = preview(b, first, last),
                    preview_truncated = range.length > 32})
            end
            report.changed_bytes = report.changed_bytes + found.changed_bytes
        elseif type(a) ~= 'table' and type(b) ~= 'table' then
            if a ~= b then add({path = fieldPath, kind = 'value',
                before = summary(a, fieldType), after = summary(b, fieldType)}) end
        else
            assert(type(a) == 'table' and type(b) == 'table', 'incompatible decoded field: ' .. fieldPath)
            assert(remaining > 0, 'comparison depth exceeded at ' .. fieldPath .. '; narrow the path or increase depth')
            if array then
                for i = 1, math.max(#a, #b) do
                    compare(a[i], b[i], fieldType, false, fieldPath .. '[' .. i .. ']', remaining - 1)
                end
            else
                for _, field in ipairs(snapshot.fields(fieldType)) do
                    local childPath = fieldPath == '' and field.name or fieldPath .. '.' .. field.name
                    compare(a[field.name], b[field.name], field.type, field.repeated, childPath, remaining - 1)
                end
            end
        end
    end
    compare(before, after, kind, repeated, path, depth)
    report.before_present, report.after_present = before ~= nil, after ~= nil
    report.difference_count = #report.differences
    report.complete = true
    return report
end
return M
-- )EOF"
