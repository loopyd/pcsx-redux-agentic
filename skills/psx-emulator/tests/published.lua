local m,snapshot,f=require 'support',require 'snapshot',require 'responses'
m.begin('',function()
    PCSX.pauseEmulator();local state=snapshot.live();local r,c=state.registers,state.cdrom
    f.seed(state,{0x3c081f80,0x35081800,0x24090001,0xa1090001,0xa1090000,0x910a0003,0,0x314a001f,0x1140fffc,0,0x2409001f,0xa1090003,0xa1000000,0x240b7fff,0x256bffff,0x1560fffe,0,0,0x08004012,0})
    c.currentPosition={m=0,s=3,f=0};c.status=0;c.motorOn=true
    f.writeSeed(state,{domain='guest GetStat literal command, native response queue publication, IRQ polling and acknowledgment',entry=r.pc,stop=0x80010048,
        exclusions=m.array({'old seeded standalone PLAY autopause callback','old restore prev/SubQ normalization expectations'})})
end)
