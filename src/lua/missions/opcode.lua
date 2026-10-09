-- lualoader, R"EOF(--
local M = {}
local names={'Sync','Nop','Setloc','Play','Forward','Backward','ReadN','Standby','Stop','Pause','Init','Mute','Demute',
    'Setfilter','Setmode','Getparam','GetlocL','GetlocP','SetSession','GetTN','GetTD','SeekL','SeekP','Unused17','Unused18',
    'Test','ID','ReadS','Reset','GetQ','ReadTOC'}
local arguments={0,0,3,-1,0,0,0,0,0,0,0,0,0,2,1,0,0,0,1,0,1,0,0,0,0,-1,0,0,0,2,0}
function M.describe(opcode)
    assert(type(opcode)=='number' and opcode>=0 and opcode<=255 and opcode%1==0,'invalid native opcode')
    return {opcode=opcode,name=names[opcode+1] or 'OutOfRange',arguments=arguments[opcode+1],
        handler=opcode<31 and opcode~=23 and opcode~=24}
end
function M.verify(record)
    local value=M.describe(record.opcode)
    assert(value.handler and (not record.start or value.arguments<0 or value.arguments==record.queue.size),
        'handler entry disagrees with native dispatch catalogue')
    return value
end
return M
-- )EOF"
