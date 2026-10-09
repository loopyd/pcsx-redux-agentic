local m,snapshot,f=require 'support',require 'snapshot',require 'responses'
m.begin('branch',function()
    PCSX.pauseEmulator();local branch=m.argument('branch','disabled')
    assert(({disabled=true,outside=true,edge=true,asserted=true,latched=true})[branch],
        'wire-3 decoded fixture is a native SPU worker domain; old CD inactive callback slot12 was removed')
    local state=snapshot.live();local r,spu=state.registers,state.spu
    f.seed(state,{0x240b0020,0x256bffff,0x1560fffe,0,0,0x08004005,0})
    spu.ctrl=branch=='disabled' and 0 or 64;spu.irq=({outside=256,edge=255})[branch] or 0
    local ports=assert(spu.ports);assert(#ports==512,'unexpected SPU ports')
    spu.ports=ports:sub(1,0x1aa)..string.char(spu.ctrl,0)..ports:sub(0x1ad)
    local hardware=state.memory.hardware;local latch=branch=='latched' and '\4\2\0\0' or string.rep('\0',4)
    state.memory.hardware=hardware:sub(1,0x1070)..latch..string.rep('\0',4)..hardware:sub(0x1079)
    f.writeSeed(state,{domain='actual SPU decoded-buffer producer and atomic CPU handoff',branch=branch,entry=r.pc,stop=0x80010014,
        control_port=spu.ctrl,irq_address=spu.irq,exclusions=m.array({'removed CD decoded scheduler slot12','CD playing flag as worker prerequisite','worker timestamp/DSP/race/deadlock/performance fidelity'})})
end)
