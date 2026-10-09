-- lualoader, R"EOF(--
local c,m = require 'controller',require 'support'
local M={}
function M.new(controller)
    local accesses=c.closed(controller,{[8]=true,[9]=true,[15]=true},function(entry,frame)
        entry.before={lid_open=frame.before.lid_open,close_scheduled=frame.before.lid_close_scheduled,close_cycle=frame.before.lid_close_cycle}
        entry.after={lid_open=frame.after.lid_open,close_scheduled=frame.after.lid_close_scheduled,close_cycle=frame.after.lid_close_cycle}
    end,'native lid transitions, guest-cycle close deadlines and actual ISO rescans')
    local predicates=c.collect(function(kind,phase) return kind==46 and phase==15 end,function(record,row)
        local frame=assert(controller.current(),'native lid predicate lacks scope');local d=c.data(row)
        assert(d[1]<=1 and d[2]<=1,'invalid native lid state');record.access=frame.id
        record.lid_open=d[1]~=0;record.close_scheduled=d[2]~=0;record.guest_close_cycle=d[3]
    end)
    return {push=function(row,payload) accesses.push(row,payload);predicates.push(row,payload) end,
        finish=function() local r=accesses.finish();r.decisions=predicates.finish().records;return r end}
end
return M
-- )EOF"
