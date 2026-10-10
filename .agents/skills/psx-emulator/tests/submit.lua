local m,snapshot,f=require 'support',require 'snapshot',require 'responses'
m.begin('scenario,mode,value',function()
    PCSX.pauseEmulator();local scenario=m.argument('scenario','all');assert(scenario=='all' or scenario=='mode','unknown submission fixture')
    local state=snapshot.live();local r,c=state.registers,state.cdrom
    f.seed(state,{})
    c.mode=m.integer('mode',0,0,255);c.readingState=0;c.readingType=0;c.status=0
    c.currentPosition={m=0,s=2,f=0};c.seekPosition={m=0,s=2,f=0}
    c.atvPending='\68\51\34\17'
    local program={0x3c081f80,0x35081800};local writes={}
    local function store(port,value)
        program[#program+1]=0x24090000+value;program[#program+1]=0xa1090000+port
        writes[#writes+1]={port=port,value=value}
    end
    local function bank(n) store(0,n) end
    local function clear() bank(1);store(3,64);bank(0) end
    local function command(op,params)
        clear()
        if params then for _,value in ipairs(params) do store(2,value) end end
        store(1,op)
    end
    if scenario=='mode' then command(14,{m.integer('value',64,0,255)})
    else
        bank(1);store(2,6);bank(2);store(2,9);bank(3);store(1,254);bank(0)
        command(9)
        command(4);command(0);command(6);command(27);command(10);command(28);command(255)
        for _,values in ipairs({{0,2,0x16},{0,2,0x17},{0,1,0x74},{0x99,0x59,0x74},
                {0x9a,2,0},{0xa0,2,0},{0,0x60,0},{0,0x5a,0},{0,2,0x75},{0,2,0x0a}}) do
            command(2,values)
        end
        for _,value in ipairs({64,64,65,64,0,192}) do command(14,{value}) end
    end
    program[#program+1]=0x240b2000;program[#program+1]=0x256bffff
    program[#program+1]=0x1560fffe;program[#program+1]=0
    local stop=r.pc+#program*4
    program[#program+1]=0x08000000+math.floor(stop/4)%0x4000000;program[#program+1]=0
    f.seed(state,program)
    f.writeSeed(state,{domain='actual guest bank/parameter/command writes with 16-byte queues',scenario=scenario,entry=r.pc,stop=stop,writes=m.array(writes),
        exclusions=m.array({'old irq+100/repeated delay preconditions','old independent reading/play streams','legacy eight-byte shared parameters'})})
end)
