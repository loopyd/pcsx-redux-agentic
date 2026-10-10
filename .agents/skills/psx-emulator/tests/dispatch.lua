local m,snapshot,f=require 'support',require 'snapshot',require 'responses'
m.begin('opcode,target,mode,params,count,reading',function()
    PCSX.pauseEmulator();local state=snapshot.live();local r,c=state.registers,state.cdrom
    local op=m.integer('opcode',1,0,30);local description=require('opcode').describe(op)
    assert(description.handler,'literal opcode has no native continuation handler')
    local params=m.argument('params','');assert(#params<=32 and #params%2==0 and not params:find('[^0-9a-fA-F]'),'invalid fixture params')
    local values={};for pair in params:gmatch('..') do values[#values+1]=string.char(tonumber(pair,16)) end
    local count=m.integer('count',#values,0,16);assert(count==#values,'fixture active prefix must match provided bytes')
    local cycle=f.seed(state,{0x240b0020,0x256bffff,0x1560fffe,0,0,0x08004005,0});local target=cycle+m.integer('target',64,1,128)
    r.schedule_mask=8;r.schedule_targets[4]='#'..m.exact(target)
    c.commandExecuting={payload=table.concat(values)..string.rep('\0',16-count),payload_size=count,payload_index=0,value=op,has_value=true,value_read=false,hit_max=false}
    c.mode=m.integer('mode',0,0,255);c.readingState=m.integer('reading',0,0,2)
    c.currentPosition={m=0,s=2,f=0};c.seekPosition={m=0,s=2,f=0};c.status=0
    f.writeSeed(state,{domain='literal command continuation at CDRCOMMANDS slot3',opcode=op,entry=r.pc,stop=0x80010014,scheduled_cycle=m.exact(target),
        exclusions=m.array({'old opcode+100 delayed commands','removed repeated/e_cycle/drive/seeked fields','old command shared parameter array'})})
end)
