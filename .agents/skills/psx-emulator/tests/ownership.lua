local f,c=require 'responses',require 'controller'
local j=f.journal(function(j) f.scope(j,13,0,0,function() f.scope(j,5,1,1,function(id)
    j.emit(36,1,{1,1,0},id,id,f.queue({value=1,has_value=true}));j.emit(36,2,{1,1,0},id,id)
end,nil,nil,0) end) end)
assert(#f.scan(j).accesses==2)
for _,row in ipairs(j.rows) do if row.kind==45 and row.device==1 and row.data[0]==5 then row.related=row.related+1 end end
f.reject(function() f.scan(j) end,'scope exit')
local bad=f.journal();bad.status.bytes=bad.status.bytes+1;f.reject(function() f.scan(bad) end,'boundaries')
print('native scope parent/depth ownership and exact payload coverage refusals passed')
