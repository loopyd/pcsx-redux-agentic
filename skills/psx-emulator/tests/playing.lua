local m,snapshot,f=require 'support',require 'snapshot',require 'responses'
m.begin('branch',function()
    PCSX.pauseEmulator();local branch=m.argument('branch','inactive')
    assert(({inactive=true,playing=true,autopause=true,report=true,seekplay=true})[branch],
        'wire-3 PLAY fixture supports inactive/playing/autopause/report/seekplay; removed callback branches are not aliases')
    local state=snapshot.live();local r,c=state.registers,state.cdrom
    local cycle=f.seed(state,{0x240b0020,0x256bffff,0x1560fffe,0,0,0x08004005,0})
    r.schedule_mask=16;r.schedule_targets[5]='#'..m.exact(cycle+64)
    c.readingState=branch=='seekplay' and 1 or 0;c.readingType=branch=='inactive' and 0 or 3
    c.status=branch=='inactive' and 0 or (branch=='seekplay' and 2 or 3)
    c.currentPosition={m=0,s=3,f=0};c.seekPosition={m=0,s=3,f=1}
    c.autoPause=branch=='autopause';c.report=branch=='report';c.playTrack=1;c.muted=branch~='report';c.playStartCycle=0
    c.atv='\128\0\128\0';c.atvPending=c.atv
    f.writeSeed(state,{domain='CDREAD slot4 actual PlayingCDDA/seek-to-play path',branch=branch,entry=r.pc,stop=0x80010014,
        exclusions=m.array({'removed standalone PLAY callback slot14','removed track_changed/prev/end-sector fields','old endpoint/right/report numerical assertions require actual fork media receipts'})})
end)
