local f,submission=require 'responses',require 'submission'
local function fixture(prefix,value)
    return f.journal(function(j)
        local before={queues={command={payload=prefix}}}
        local after={queues={command={payload=prefix..(#prefix<16 and string.char(value) or '')}}}
        f.scope(j,4,0x1f801802,value,nil,before,after)
        f.scope(j,4,0x1f801801,9,nil,{queues={command={payload=prefix,has_value=true}}},
            {submission=9,queues={command={id=10,value=9,payload=prefix,has_value=true}}})
    end)
end
for _,prefix in ipairs({'',string.rep('a',15),string.rep('a',16)}) do
    local r=f.scan(fixture(prefix,255),function(c) return submission.new(c) end)
    assert(r.accesses[1].parameter_appended==(#prefix<16) and r.accesses[2].overwrote_pending)
end
local bad=fixture('',2);for _,row in ipairs(bad.rows) do if row.kind==35 and row.device==1 and row.data[1]==0x1f801802 then row.payload=f.context({}) end end
f.reject(function() f.scan(bad,function(c) return submission.new(c) end) end)
print('native command overwrite and capped 16-byte parameter progression passed')
