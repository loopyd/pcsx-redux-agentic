-- lualoader, R"EOF(--
local c, opcode = require 'controller', require 'opcode'
local M={}
function M.new(controller)
    return c.closed(controller,{[5]=true},function(entry,frame)
        local command=assert(frame.command,'native command lacks handler entry')
        assert(frame.address==command.opcode and frame.value==(command.start and 1 or 0) and frame.returned and
            frame.result<=1 and frame.command_return==frame.result,'native command return boundary differs from entry')
        entry.command=opcode.verify(command);entry.start=command.start;entry.queue=command.queue
        entry.continuation_requested=frame.result~=0
        entry.before={status=frame.before.status,reading_state=frame.before.reading_state,reading_type=frame.before.reading_type}
        entry.after={status=frame.after.status,reading_state=frame.after.reading_state,reading_type=frame.after.reading_type}
    end,'literal native handler entries and continuation returns; no removed callback simulation')
end
return M
-- )EOF"
