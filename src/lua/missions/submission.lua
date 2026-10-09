-- lualoader, R"EOF(--
local c, m = require 'controller', require 'support'
local M={}
function M.new(controller)
    return c.closed(controller,{[4]=true},function(entry,frame)
        local before,after=frame.before,frame.after
        entry.bank=before.register_bank
        local port=frame.address%4;entry.port=port
        if port==1 and before.register_bank==0 then
            assert(after.queues.command.value==frame.value and after.queues.command.has_value and
                after.queues.command.size==before.queues.command.size and
                after.queues.command.payload_hex==before.queues.command.payload_hex and
                after.identities.latest_submission~='0' and after.identities.latest_submission~=before.identities.latest_submission,
                'native command overwrite differs from write1')
            entry.command=after.queues.command;entry.submission=after.identities.latest_submission
            entry.overwrote_pending=before.queues.command.has_value
        elseif port==2 and before.register_bank==0 then
            local b,a=before.queues.command,after.queues.command
            assert(a.size==math.min(b.size+1,16) and a.payload_hex==b.payload_hex..
                (b.size<16 and string.format('%02x',frame.value) or ''),'native parameter append differs from bounded queue')
            entry.parameter_appended=b.size<16;entry.parameter_count=a.size
        elseif port==0 then assert(after.register_bank==frame.value%4,'native bank select mismatch') end
    end,'native register-bank writes, command replacement and capped 16-byte parameter appends')
end
return M
-- )EOF"
