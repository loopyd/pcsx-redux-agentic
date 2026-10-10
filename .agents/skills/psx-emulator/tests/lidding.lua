local m,snapshot,f=require 'support',require 'snapshot',require 'responses'
m.begin('branch',function()
    PCSX.pauseEmulator();local branch=m.argument('branch','closed')
    assert(({closed=true,open=true,scheduled=true,expired=true})[branch],
        'wire-3 lid fixture supports closed/open/scheduled/expired; removed drive-state callback has no equivalent')
    local state=snapshot.live();local r,c=state.registers,state.cdrom
    local cycle=f.seed(state,{0x3c081f80,0x35081800,0x24090001,0xa1090001,0x910a0000,0,0,0x08004007,0})
    c.lidOpen=branch~='closed';c.wasLidOpened=c.lidOpen;c.lidCloseScheduled=branch=='scheduled' or branch=='expired'
    c.lidCloseAtCycles=tonumber((cycle+(branch=='expired' and 0 or 100000))%4294967296)
    c.motorOn=true
    f.writeSeed(state,{domain='guest GetStat/read status query calls actual cycle-based isLidOpen',branch=branch,entry=r.pc,stop=0x8001001c,
        exclusions=m.array({'removed scheduled lid callback slot13','removed drive-state standby/rescan/prepare domains','wall-clock close assumptions'})})
end)
