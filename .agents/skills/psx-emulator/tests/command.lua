local f,command=require 'responses',require 'command'
local function fixture(start,result)
    return f.journal(function(j)
        f.scope(j,start and 13 or 14,0,0,function(parent)
            f.scope(j,5,1,start and 1 or 0,function(id)
                j.emit(36,1,{1,start and 1 or 0,0},id,id,f.queue({value=1,has_value=true}))
                j.emit(36,2,{1,start and 1 or 0,result},id,id)
            end,nil,nil,result)
        end)
    end)
end
for _,start in ipairs({true,false}) do for result=0,1 do
    local report=f.scan(fixture(start,result),function(c) return command.new(c) end)
    assert(#report.accesses==1 and report.accesses[1].start==start and report.accesses[1].continuation_requested==(result==1))
end end
local bad=fixture(true,0);for _,r in ipairs(bad.rows) do if r.kind==36 and r.device==2 then r.data[2]=1 end end
f.reject(function() f.scan(bad,function(c) return command.new(c) end) end,'return boundary')
print('native handler start/continuation pairing and mismatched return refusal passed')
