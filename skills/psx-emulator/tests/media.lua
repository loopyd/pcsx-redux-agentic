local f,media,image=require 'responses',require 'media',require 'image'
local function fixture(success)
    return f.journal(function(j)
        f.scope(j,6,0,0,function(owner)
            j.emit(40,0,{1,512,150,0,0},10,owner)
            j.emit(40,2,{7,2352,0,0,0,0,12},10,owner)
            j.emit(40,1,{1,success,0},10,owner)
            j.emit(40,6,{6,512,1},owner,owner,'\1\1\0\0\0\0\2\0')
        end)
    end)
end
assert(f.scan(fixture(1),function(c) return media.new(c) end).lookups[1].native_count==nil)
local r=f.scan(fixture(1),function(c) return media.new(c) end);assert(r.lookups[1].success and r.lookups[1].outcome.native_count=='2352' and #r.queries==1)
local bad=fixture(1);for _,row in ipairs(bad.rows) do if row.kind==40 and row.device==1 then row.request=row.request+1 end end
f.reject(function() f.scan(bad,function(c) return media.new(c) end) end,'matching backend')
local j=f.journal();local row=j.rows[2];local decoded=image.decode(row,row.payload);assert(decoded.schema=='psx.cd-image/v3')
row.data[0]=2;f.reject(function() image.decode(row,row.payload) end,'layout')
print('actual native backend/query receipts and revisioned image refusal passed')
