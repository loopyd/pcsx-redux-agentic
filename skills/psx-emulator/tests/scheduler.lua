local f,scheduler,m=require 'responses',require 'scheduler',require 'support'
local scheduled,dispatched
local j=f.journal(function(j)
    f.scope(j,4,0x1f801800,0,function(id)
        local target=j.cycle+64
        local payload=f.u64(0)..f.u64(0)..f.u64(0)..f.u64(target)..f.u32(0)..f.u32(8)..f.u32(0x3f800000)..f.u32(0)
        scheduled=j.emit(37,1,{3,0},id,0,payload)
        local pass=j.next();j.emit(37,3,{8,8},pass)
        local tested=f.u64(pass)..f.u64(id)..f.u64(target)..f.u64(target)..f.u32(8)..f.u32(8)..f.u32(0x3f800000)..f.u32(0)
        dispatched=j.emit(37,5,{3,1},id,id,tested)
    end)
end)
local result=f.scan(j,scheduler.new)
assert(#result.scale_bits==11 and #result.slots==4 and result.passes==1)
assert(result.records[5].previous_owner=='0' and result.records[7].previous_owner=='1')
local old=scheduled.payload;scheduled.payload=f.put(old,40,f.u32(0));f.reject(function() f.scan(j,scheduler.new) end,'scheduler reserved/scale mismatch');scheduled.payload=old
old=dispatched.payload;dispatched.payload=f.put(old,8,f.u64(0));f.reject(function() f.scan(j,scheduler.new) end,'scheduler replaced wrong native owner');dispatched.payload=old
old=dispatched.payload;dispatched.payload=f.put(old,0,f.u64(0));f.reject(function() f.scan(j,scheduler.new) end,'scheduler target-test pass mismatch');dispatched.payload=old
local removed=f.journal();table.remove(removed.rows);f.pack(removed);f.reject(function() f.scan(removed,scheduler.new) end,'missing CD schedule boundary')
assert(m.exact(j.cycle)=='9007199254740993')
print('four actual callback owners, eleven scale patterns, pass ancestry and boundary refusals passed')
