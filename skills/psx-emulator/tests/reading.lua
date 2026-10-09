local m,snapshot,f=require 'support',require 'snapshot',require 'responses'
m.begin('branch',function()
    PCSX.pauseEmulator();local branch=m.argument('branch','inactive')
    assert(({inactive=true,seeking=true,reading=true,speed=true})[branch],
        'wire-3 READ fixture supports inactive/seeking/reading/speed; old busy/retry/error/location meanings were removed')
    local state=snapshot.live();local r,c=state.registers,state.cdrom
    local cycle=f.seed(state,{0x240b0020,0x256bffff,0x1560fffe,0,0,0x08004005,0})
    r.schedule_mask=16;r.schedule_targets[5]='#'..m.exact(cycle+64)
    c.readingState=branch=='inactive' and 0 or (branch=='reading' and 2 or 1)
    c.readingType=branch=='inactive' and 0 or 1;c.status=0;c.speedChanged=branch=='speed'
    c.currentPosition={m=0,s=2,f=0};c.seekPosition={m=0,s=2,f=0}
    c.realtime,c.subheaderFilter,c.xaFirstSector=false,false,true
    f.writeSeed(state,{domain='actual CDREAD slot4 Seeking/Reading/Idle transition',branch=branch,entry=r.pc,stop=0x80010014,
        scheduled_cycle=m.exact(cycle+64),exclusions=m.array({'old response-busy retry','old hardware-IRQ retry','old backend-error switch','removed location_changed field'})})
end)
