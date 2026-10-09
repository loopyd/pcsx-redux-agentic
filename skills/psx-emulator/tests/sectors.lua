local f,sectors=require 'responses',require 'sectors'
local function fixture(origin)
    return f.journal(function(j)
        j.emit(41,2,{1,4,3},12,0,'abc')
        local span=f.u32(3)..f.u32(0)..f.u32(4)..f.u32(3)..f.u32(0x1f801802)..f.u32(1)..string.rep('\0',8)..
            f.u32(0)..f.u32(3)..f.u64(origin)..'abc'
        j.emit(42,0,{4,3,0x1f801802,1},0,0,span)
    end)
end
local j=fixture(12);local tracker=sectors.new({mutations=4,bytes=4})
for _,row in ipairs(j.rows) do tracker.push(row,row.payload) end
local r=tracker.finish();assert(#r.consumptions==1 and r.consumptions[1].origins[1].mutation=='12' and #r.mutations==1)
f.reject(function() local t=sectors.new({mutations=4,bytes=4});for _,row in ipairs(fixture(13).rows) do t.push(row,row.payload) end;t.finish() end,'unknown FIFO')
f.reject(function() local t=sectors.new({mutations=4,bytes=2});for _,row in ipairs(j.rows) do t.push(row,row.payload) end end,'budget')
print('native bulk consumed-span accounting, origin association and bounded mutation refusals passed')
