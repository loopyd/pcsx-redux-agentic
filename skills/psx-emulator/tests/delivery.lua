local f,delivery=require 'responses',require 'delivery'
for _,index in ipairs({0,1,15,16}) do
    local expected=index%16<2 and (index%16==0 and 17 or 34) or 0
    local journal=f.journal(function(j)
        f.scope(j,3,0x1f801801,0,nil,{queues={head={payload='\17\34',index=index}}},
            {queues={head={payload='\17\34',index=index%16+1,hit_max=index%16+1==2}}},expected)
    end)
    assert(f.scan(journal,function(c) return delivery.new(c) end).accesses[1].response==expected)
end
for _,read in ipairs({true,false}) do
    local j=f.journal(function(j) f.scope(j,3,0x1f801803,0,nil,{bank=1,queues={head={value=5,value_read=read}}},nil,read and 224 or 229) end)
    assert(f.scan(j,function(c) return delivery.new(c) end).accesses[1].cause==(read and 0 or 5))
end
local bad=f.journal(function(j) f.scope(j,3,0x1f801803,0,nil,{bank=1,queues={head={value=5}}},nil,224) end)
f.reject(function() f.scan(bad,function(c) return delivery.new(c) end) end,'cause read')
print('native payload wrap16, retained stored cause and malformed result refusal passed')
