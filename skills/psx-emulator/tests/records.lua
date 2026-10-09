local f,events=require 'responses',require 'events'
local j=f.journal(function(j) f.scope(j,3,0x1f801803,0,nil,{bank=1,queues={head={value=5}}},nil,229) end)
for _,row in ipairs(j.rows) do local kind,raw,fields=events.describe(row);assert(kind and raw and fields) end
local bad=f.journal();bad.rows[1].data[0]=2;f.reject(function() events.describe(bad.rows[1]) end)
local row={kind=42,device=0,length=51,data={[0]=0,[1]=3,[2]=0x1f801802,[3]=1,[4]=0,[5]=0,[6]=0,[7]=0}}
local kind,raw,fields=events.describe(row);assert(fields.byte_count==3)
row.length=50;f.reject(function() events.describe(row) end)
print('revisioned native row discriminants, bulk extent accounting and obsolete-wire refusal passed')
