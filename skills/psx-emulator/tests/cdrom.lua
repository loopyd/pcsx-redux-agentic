local f,c=require 'responses',require 'controller'
for index=0,16 do
    local queue=c.queue(f.queue({payload=string.rep('a',16),index=index,value=5}),0)
    assert(queue.index==index and queue.value==5 and queue.qualified_prefix_bytes==16)
end
local before=c.context(f.context({bank=1,queues={head={value=5,value_read=true}}}))
assert(before.register_bank==1 and before.queues.head.value==5 and before.queues.head.value_read)
f.reject(function() c.queue(f.put(f.queue(),3,'\17'),0) end)
print('fork CD queue retained bytes, independent size/index and cause-read readiness representation passed')
