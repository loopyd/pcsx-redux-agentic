local ffi,c,m = require 'ffi',require 'controller',require 'support'
local M={}
function M.u32(n) return string.char(n%256,math.floor(n/256)%256,math.floor(n/65536)%256,math.floor(n/16777216)%256) end
function M.u64(n)
    n=ffi.new('uint64_t',n or 0);local bytes={}
    for i=1,8 do bytes[i]=string.char(tonumber(n%256));n=n/256 end
    return table.concat(bytes)
end
function M.put(bytes,offset,value) return bytes:sub(1,offset)..value..bytes:sub(offset+#value+1) end
function M.queue(options)
    options=options or {};local id=options.id or 0;local prefix=options.payload or '';assert(#prefix<=16)
    local flags=8+(id==0 and 16 or 0)+(options.has_value and 1 or 0)+(options.value_read and 2 or 0)+(options.hit_max and 4 or 0)
    return string.char(flags,options.value or 0,#prefix,options.index or 0)..string.rep('\0',4)..M.u64(id)..prefix..string.rep('\0',16-#prefix)
end
function M.context(options)
    options=options or {};local bytes=string.rep('\0',384)
    for offset,value in pairs({[0]=3,[4]=384,[8]=1,[32]=options.flags or 0,[36]=options.fifo_index or 0,
        [40]=options.fifo_size or 0,[44]=options.fifo_pending or 0,[48]=options.play_start or 0,[52]=options.close_cycle or 0,
        [304]=options.mask or 0,[308]=options.chcr or 0}) do bytes=M.put(bytes,offset,M.u32(value)) end
    bytes=M.put(bytes,16,M.u64(options.epoch or 7));bytes=M.put(bytes,24,M.u64(options.audio_epoch or 0))
    bytes=M.put(bytes,56,M.u64(options.seed or 0));bytes=M.put(bytes,64,string.char(options.bank or 0,options.reading_state or 0,
        options.reading_type or 0,options.status or 0,options.speed or 0,options.span or 0,options.interrupt_mask or 31,
        options.play_track or 0,options.mode or 0,options.filter_file or 0,options.filter_channel or 0))
    bytes=M.put(bytes,75,options.position or '\0\2\0');bytes=M.put(bytes,78,options.seek_position or '\0\2\0')
    bytes=M.put(bytes,81,options.attenuation or '\128\0\128\0');bytes=M.put(bytes,85,options.attenuation or '\128\0\128\0')
    local queues=options.queues or {}
    for i,name in ipairs({'command','executing','head','tail'}) do bytes=M.put(bytes,112+(i-1)*32,M.queue(queues[name])) end
    for i,name in ipairs({'root','access','stream','mutation','media','feed','submission','pass'}) do bytes=M.put(bytes,232+i*8,M.u64(options[name] or 0)) end
    bytes=M.put(bytes,312,M.u64(options.lowest or 0))
    for i,slot in ipairs({2,3,4,10}) do
        local schedule=(options.schedules or {})[slot] or {};bytes=M.put(bytes,304+i*16,M.u64(schedule.target));bytes=M.put(bytes,312+i*16,M.u64(schedule.owner))
    end
    return bytes
end
function M.image(phase)
    local words={};for i=1,716 do words[i]=0 end;words[2],words[15]=100,1
    local bytes={};for i,n in ipairs(words) do bytes[i]=M.u32(n) end;return table.concat(bytes)
end
function M.journal(body,options)
    options=options or {};local j={rows={},serial=0,stack={},cycle=ffi.new('uint64_t',9007199254740992)+1,options=options}
    function j.next() j.serial=j.serial+1;return j.serial end
    function j.emit(kind,phase,values,request,related,payload)
        local data={};for n=0,7 do data[n]=(values or {})[n+1] or 0 end
        local row={kind=kind,device=phase,data=data,request=ffi.new('uint64_t',request or 0),related=ffi.new('uint64_t',related or 0),cycle=j.cycle,payload=payload or ''}
        j.rows[#j.rows+1]=row;return row
    end
    local settings={};for i=1,14 do settings[i]=M.u64((i==6 or i==14) and 1 or 0) end
    settings[6]=M.u64(1);settings[8]=M.u64(2);settings[14]=M.u64(1)
    local scales={};for i=1,11 do scales[i]=M.u32(0x3f800000) end
    j.emit(34,2,{3,33868800,14,11,0,0,1,1},0,0,table.concat(settings)..table.concat(scales))
    j.emit(47,0,{3,716,0,100},1,0,M.image(0))
    j.emit(34,0,{3,384},0,0,M.context(options));j.emit(41,0,{1,0,0,0,2352},0,0,string.rep('\0',2352))
    for _,slot in ipairs({2,3,4,10}) do j.emit(37,6,{slot,0,0,0,0x3f800000}) end
    if body then body(j) end
    j.emit(47,1,{3,716,0,100},1,0,M.image(1));j.emit(34,1,{3,384},0,0,M.context(options))
    j.emit(41,1,{1,0,0,0,2352},0,0,string.rep('\0',2352))
    for _,slot in ipairs({2,3,4,10}) do j.emit(37,7,{slot,0,0,0,0x3f800000}) end
    return M.pack(j)
end
function M.pack(j)
    local chunks,size={},0
    for i,row in ipairs(j.rows) do row.sequence=ffi.new('uint64_t',i);row.offset=size;row.length=#row.payload;chunks[i]=row.payload;size=size+row.length end
    j.payload=table.concat(chunks);j.status={version=1,state=2,failures=0,dropped=0,firstFailure=0,events=#j.rows,bytes=size,beginCycle=j.cycle,endCycle=j.cycle}
    j.h={status=function() return j.status end,record=function(i) return assert(j.rows[i+1]) end,
        bytes=function(offset,length) return j.payload:sub(offset+1,offset+length) end}
    return j
end
function M.scope(j,role,address,value,body,before,after,result)
    local id,parent=j.next(),j.stack[#j.stack];local depth=#j.stack+1
    j.stack[depth]=id;local root=j.stack[1];local kind=role<=4 and 35 or 45;local width=role<=4 and 8 or 0
    local function context(options)
        options=options or {};options.root,options.access=id==root and id or root,id;return M.context(options)
    end
    j.emit(kind,0,{role,address or 0,width,value or 0,0,0,depth},id,parent or 0,context(before))
    if body then body(id) end
    local returned=role==1 or role==3 or role==5
    j.emit(kind,1,{role,address or 0,width,returned and (result or 0) or (value or 0),0,returned and 1 or 0,depth},id,parent or 0,context(after or before))
    table.remove(j.stack);return id
end
function M.scan(j,factory)
    local controller=c.new(j.status);local tracker=factory and factory(controller) or controller
    for _,row in ipairs(j.rows) do controller.push(row,row.payload);if tracker~=controller then tracker.push(row,row.payload) end end
    local result=tracker.finish();if tracker~=controller then controller.finish() end;return result
end
function M.reject(callback,fragment)
    local ok,err=pcall(callback);assert(not ok and (not fragment or tostring(err):find(fragment,1,true)),tostring(err))
end
function M.identity(callback)
    local previous=m.buildIdentity;m.buildIdentity=function() return {binary_sha256=string.rep('a',64),source_manifest_sha256=string.rep('b',64),authority='synthetic parser fixture'} end
    local result={pcall(callback)};m.buildIdentity=previous;assert(result[1],result[2]);return unpack(result,2)
end
function M.seed(state,words)
    local r,c=state.registers,state.cdrom
    r.pc,r.cp0[13],r.next_is_delay_slot=0x80010000,0,false
    for _,name in ipairs({'delay_slot_info_1','delay_slot_info_2'}) do r[name]=r[name] or {};r[name].active,r[name].pc_active=false,false end
    r.schedule_mask=0;for i=1,32 do r.schedule_targets[i]='#0' end
    for _,name in ipairs({'commandFifo','commandExecuting','responseFifo0','responseFifo1'}) do
        c[name]={payload=string.rep('\0',16),value=0,value_read=false,has_value=false,hit_max=false,payload_size=0,payload_index=0}
    end
    c.registerAddress,c.interruptCauseMask=0,31;c.lidOpen,c.lidCloseScheduled=false,false
    local cycle=m.uint64(tostring(r.cycle):gsub('^#',''));assert(cycle<m.uint64('18446744073708500000'),'fixture cycle cannot schedule safely')
    local bytes=ffi.string(ffi.new('uint32_t[?]',#words,words),#words*4)
    state.memory.ram=state.memory.ram:sub(1,0x10000)..bytes..state.memory.ram:sub(0x10001+#bytes)
    return cycle
end
function M.writeSeed(state,report)
    report.schema='psx.native-cd-fixture/v2';report.cd_observation_version=3;report.build_identity=m.buildIdentity()
    report.provenance='host-edited v6 preconditions; native execution and observation gates remain separate'
    m.write('state.pbuf',require('pb').encode('SaveState',state));m.report('fixture.json',report);m.finish()
end
if ...~='responses' then
    local queue=c.queue(M.queue({value=123}),0);assert(queue.value==123 and not queue.has_value and queue.prehistory)
    local journal=M.journal();local result=M.scan(journal);assert(result.initial.version==3 and #result.responses==0)
    M.reject(function() c.queue(M.put(M.queue(),2,'\17'),0) end,'invalid wire-3 queue')
    M.reject(function() c.context(M.put(M.context(),0,M.u32(2))) end,'invalid wire-3 CD context')
    print('wire-3 synthetic queue/context bounds and old-version refusal passed; no native fidelity claim')
end
return M
