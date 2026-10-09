-- lualoader, R"EOF(--
local bit,c = require 'bit',require 'controller'
local M={}
function M.new(controller)
    return c.closed(controller,{[3]=true,[4]=true,[6]=true},function(entry,frame)
        local before,after=frame.before,frame.after;local port=frame.address%4
        if frame.role==6 then
            entry.before={reading_state=before.reading_state,reading_type=before.reading_type,position=before.position}
            entry.after={reading_state=after.reading_state,reading_type=after.reading_type,position=after.position}
        elseif frame.role==3 and port==1 then
            local b,a=before.queues.head,after.queues.head;local index=b.index%16
            local expected=index<b.size and tonumber(b.payload_hex:sub(index*2+1,index*2+2),16) or 0
            assert(frame.returned and frame.result==expected and a.index==index+1 and
                a.hit_max==(b.hit_max or a.index==b.size),'native result read differs from queue progression')
            entry.response=expected;entry.before_queue=b;entry.after_queue=a
        elseif frame.role==3 and port==3 then
            local expected=before.register_bank%2==0 and before.interrupt_mask or
                (before.queues.head.value_read and 0 or before.queues.head.value)
            assert(frame.result==bit.bor(expected,0xe0),'native cause read differs from qualified stored value')
            entry.cause=expected;entry.stored_value_defined=true;entry.has_value=before.queues.head.has_value
        elseif frame.role==3 and port==2 then
            assert(frame.returned,'native FIFO read lacks return')
            if before.fifo_index==before.fifo_size then assert(frame.result==0,'empty native FIFO read differs from zero') end
            entry.value=frame.result;entry.before_index=before.fifo_index;entry.after_index=after.fifo_index
        elseif frame.role==4 and port==3 and before.register_bank==1 then
            if frame.value==7 or frame.value==31 then assert(after.queues.head.value_read,'native cause ack did not mark read');entry.ack=true
            elseif bit.band(frame.value,64)~=0 then assert(after.queues.command.size==0,'native parameter clear failed');entry.parameters_cleared=true end
        end
    end,'native result/cause/FIFO reads and acknowledgements; read delivery arithmetic remains in the native scopes and receipts')
end
return M
-- )EOF"
