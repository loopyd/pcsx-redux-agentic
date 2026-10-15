-- lualoader, R"EOF(--
local ffi, bit, m = require 'ffi', require 'bit', require 'support'
local M = {}
local zero = ffi.new('uint64_t', 0)
local slots = {2,3,4,10}
M.scheduleNames={'sio','sio1','cd-fifo','cd-commands','cd-read','gpu-dma','mdec-output-dma','spu-dma','mdec-input-dma','gpu-otc-dma','cd-dma'}
M.operations={'cpu-read','cpu-write','handler-read','handler-write','command','read','play','lid','lid-seek','dma','dma-complete','decode','fifo','continuation','iso-scan'}
function M.collect(filter, decode, finish)
    local result={records=m.array()};local tracker={}
    function tracker.push(row,payload)
        if filter(tonumber(row.kind),tonumber(row.device),row) then
            local record=M.reference(row);record.variant=tonumber(row.device);record.raw=m.array(M.data(row))
            if decode then decode(record,row,payload) end
            result.records[#result.records+1]=record
        end
    end
    function tracker.finish() if finish then finish(result) end;return result end
    return tracker
end
function M.owner(controller,row,role)
    local frame=assert(controller.current(),'native record lacks active scope')
    assert(not role or frame.role==role,'native record has wrong scope role')
    assert(m.exact(row.related)==frame.id,'native record has wrong scope owner')
    return frame
end
function M.closed(controller,roles,decode,scope)
    local result={accesses=m.array(),scope=scope};local tracker={}
    function tracker.push(row,payload)
        local frame=controller.closed()
        if frame and roles[frame.role] then
            local entry={access=frame.id,role=frame.role,parent=frame.parent,address=frame.address,operand=frame.value,
                returned=frame.returned,result=frame.result,enter=frame.enter,exit=frame.exit}
            decode(entry,frame)
            result.accesses[#result.accesses+1]=entry
        end
    end
    function tracker.finish() return result end
    return tracker
end
local booleans = {'lid_open','lid_opened','lid_close_scheduled','motor_on','speed_changed','invalid_loc_l',
    'data_requested','subheader_filter','realtime','start_playing','auto_pause','report','set_loc_pending',
    'muted','peak_flag','adpcm_muted','xa_ended','xa_first_sector','sound_map_enabled'}
function M.queue(bytes, offset)
    assert(#bytes>=offset+32, 'short native queue snapshot')
    local flags,value,size,index=bytes:byte(offset+1,offset+4)
    assert(flags<32 and bit.band(flags,8)~=0 and size<=16 and index<=16 and bytes:sub(offset+5,offset+8)=='\0\0\0\0',
        'invalid wire-3 queue snapshot')
    assert(bytes:sub(offset+17+size,offset+32)==string.rep('\0',16-size), 'nonzero unqualified queue suffix')
    local id=m.exact(m.u64le(bytes,offset+8))
    assert((id=='0')==(bit.band(flags,16)~=0), 'queue prehistory flag differs from identity')
    return {flags=flags,value=value,size=size,index=index,content_id=id,has_value=bit.band(flags,1)~=0,
        value_read=bit.band(flags,2)~=0,hit_max=bit.band(flags,4)~=0,prehistory=id=='0',
        payload_hex=m.hex(bytes:sub(offset+17,offset+16+size)),qualified_prefix_bytes=size}
end
function M.context(bytes)
    assert(#bytes==384 and m.u32le(bytes,0)==3 and m.u32le(bytes,4)==384 and m.u32le(bytes,8)==1 and
        m.u32le(bytes,12)==0 and bytes:sub(90,96)==string.rep('\0',7), 'invalid wire-3 CD context')
    local result={version=3,history_epoch=m.exact(m.u64le(bytes,16)),audio_epoch=m.exact(m.u64le(bytes,24)),
        flags=m.u32le(bytes,32),fifo_index=m.u32le(bytes,36),fifo_size=m.u32le(bytes,40),fifo_pending=m.u32le(bytes,44),
        play_start_cycle=m.u32le(bytes,48),lid_close_cycle=m.u32le(bytes,52),random_seed=m.exact(m.u64le(bytes,56))}
    assert(result.flags<2^19 and result.fifo_index<=result.fifo_size and result.fifo_size<=2352 and result.fifo_pending<=2352,
        'native CD context exceeds storage')
    for i,name in ipairs(booleans) do result[name]=bit.band(result.flags,2^(i-1))~=0 end
    local names={'register_bank','reading_state','reading_type','status','speed','read_span','interrupt_mask','play_track','mode','filter_file','filter_channel'}
    for i,name in ipairs(names) do result[name]=bytes:byte(64+i) end
    assert(result.register_bank<=3 and result.reading_state<=2 and result.reading_type<=3 and result.status<=3 and
        result.speed<=1 and result.read_span<=2, 'unknown native CD enumeration')
    result.position={bytes:byte(76,78)};result.seek_position={bytes:byte(79,81)}
    for _,position in ipairs({result.position,result.seek_position}) do assert(position[2]<60 and position[3]<75,'invalid native MSF') end
    result.attenuation=m.array({bytes:byte(82,85)});result.pending_attenuation=m.array({bytes:byte(86,89)})
    result.loc_p_hex=m.hex(bytes:sub(97,104));result.loc_l_hex=m.hex(bytes:sub(105,112))
    result.queues={command=M.queue(bytes,112),executing=M.queue(bytes,144),head=M.queue(bytes,176),tail=M.queue(bytes,208)}
    local ids={'root','access','stream','data_mutation','media_lookup','audio_feed','latest_submission','scheduler_pass'}
    result.identities={};for i,name in ipairs(ids) do result.identities[name]=m.exact(m.u64le(bytes,232+i*8)) end
    result.scheduler_mask=m.u32le(bytes,304);result.dma_chcr=m.u32le(bytes,308);result.lowest_target=m.exact(m.u64le(bytes,312))
    assert(bit.band(result.scheduler_mask,bit.bnot(0x7ff))==0,'unknown scheduler bits')
    result.schedules=m.array();for i,slot in ipairs(slots) do
        result.schedules[i]={slot=slot,target=m.exact(m.u64le(bytes,304+i*16)),owner=m.exact(m.u64le(bytes,312+i*16))}
    end
    return result
end
function M.data(row)
    local result={};for i=0,7 do result[i+1]=tonumber(row.data[i]) end;return result
end
function M.reference(row)
    return {sequence=m.exact(row.sequence),cycle=m.exact(row.cycle),request=m.exact(row.request),related=m.exact(row.related),
        payload={file='payload.bin',offset=tonumber(row.offset),bytes=tonumber(row.length)}}
end
function M.new(status)
    assert(status.version==1 and status.state==2 and status.failures==0 and status.dropped==0 and status.events<=65536 and
        status.bytes<=8388608 and status.beginCycle<=status.endCycle, 'CD controller requires complete frozen history')
    local tracker={};local stack,seen={},{};local count,offset,previous=0,0,status.beginCycle
    local result={accesses=m.array(),commands=m.array(),responses=m.array(),irqs=m.array(),contexts=m.array()}
    local first,final,environment,last,closed
    local function requireFrame(row)
        local frame=assert(stack[#stack],'native CD record has no scope')
        return frame
    end
    function tracker.push(row,payload)
        count=count+1
        assert(row.sequence==count and count<=status.events and row.cycle>=previous and row.cycle<=status.endCycle and
            row.offset==offset and #payload==tonumber(row.length) and offset+#payload<=status.bytes,'CD journal order/bounds mismatch')
        previous,offset=row.cycle,offset+#payload
        local kind,phase,d=tonumber(row.kind),tonumber(row.device),M.data(row)
        closed=nil
        if kind==34 then
            if phase==2 then
                assert(not environment and not first and #stack==0 and d[1]==3 and d[3]==14 and d[4]==11 and #payload==156+d[5]+d[6] and m.u64le(payload,56)==2 and m.u64le(payload,64)==0,
                    'invalid native CD environment')
                environment=M.reference(row);environment.clock_hz=d[2];environment.settings_version=2;environment.scale_bits=m.array()
                for i=0,10 do environment.scale_bits[i+1]=m.u32le(payload,112+i*4) end
            else
                assert((phase==0 and not first and not final) or (phase==1 and first and not final),'invalid CD context boundary')
                assert(#stack==0 and row.request==zero and row.related==zero,'nested CD boundary')
                local context=M.context(payload)
                assert(context.identities.root=='0' and context.identities.access=='0','boundary invented operation')
                if phase==0 then first=context else
                    assert(context.history_epoch==first.history_epoch and context.audio_epoch==first.audio_epoch,'CD epochs changed')
                    final=context
                end
                result.contexts[#result.contexts+1]={phase=phase,state=context,record=M.reference(row)};last=context
            end
        elseif kind==35 or (kind==45 and phase<=1) then
            assert(first and not final and phase<=1 and d[1]>=1 and d[1]<=15 and d[7]>=1 and d[7]<=16 and d[8]==0,'CD scope outside boundary')
            local context=M.context(payload);local id=m.exact(row.request)
            assert(context.history_epoch==first.history_epoch and context.audio_epoch==first.audio_epoch and context.identities.access==id,
                'CD scope context owner mismatch')
            if phase==0 then
                assert(not seen[id] and id~='0' and d[7]==#stack+1 and d[6]==0,'duplicate CD scope/depth')
                local parent=stack[#stack]
                assert(m.exact(row.related)==(parent and parent.id or '0'),'CD scope parent mismatch')
                assert(context.identities.root==(parent and stack[1].id or id),'CD scope root mismatch')
                if d[1]<=4 then assert(d[3]==8 and bit.band(d[2],0x1fffffff)>=0x1f801800 and bit.band(d[2],0x1fffffff)<=0x1f801803,'invalid CD port') end
                seen[id]=true;stack[#stack+1]={id=id,role=d[1],address=d[2],width=d[3],value=d[4],parent=m.exact(row.related),before=context,enter=M.reference(row)}
            else
                local frame=requireFrame(row)
                assert(frame.id==id and frame.role==d[1] and frame.address==d[2] and frame.width==d[3] and frame.parent==m.exact(row.related) and d[7]==#stack,
                    'unmatched CD scope exit')
                if frame.role==1 or frame.role==3 then assert(d[6]==1,'CD read has no return') end
                frame.after=context;frame.returned=d[6]~=0;frame.result=d[4];frame.exit=M.reference(row)
                table.remove(stack);result.accesses[#result.accesses+1]=frame;closed=frame
            end
            last=context
        elseif kind==36 then
            local frame=requireFrame(row);assert(frame.role==5 and m.exact(row.request)==frame.id,'handler record lacks native command scope')
            local record=M.reference(row);record.opcode=d[1];record.start=d[2]~=0;record.phase=phase
            if phase==1 then record.queue=M.queue(payload,0);frame.command=record
            else assert(phase==2 and frame.command and frame.command.opcode==d[1] and frame.command.start==(d[2]~=0),'unpaired handler return');record.continuation=d[3]~=0;frame.command_return=d[3] end
            result.commands[#result.commands+1]=record
        elseif kind==38 then
            local frame=requireFrame(row);assert(phase<=3 and #payload==64 and d[1]<=1,'invalid native response branch')
            local record=M.reference(row);record.branch=({'drop','head','tail','promote-and-tail'})[phase+1];record.native_result=d[1]~=0
            record.before={head=m.exact(m.u64le(payload,0)),tail=m.exact(m.u64le(payload,8))}
            record.after={head=m.exact(m.u64le(payload,16)),tail=m.exact(m.u64le(payload,24))};record.candidate=M.queue(payload,32)
            record.stage=({'data-ready','complete','acknowledge','end','error'})[record.candidate.value] or 'unknown'
            record.owner_access=frame.id;record.owner_role=M.operations[frame.role]
            record.stream=frame.before.identities.stream
            for i=#stack,1,-1 do
                local command=stack[i].command
                if command then
                    record.command={opcode=command.opcode,start=command.start,content_id=command.queue.content_id}
                    break
                end
            end
            if not record.command and frame.role==13 and frame.before.queues.command.has_value then
                local queue=frame.before.queues.command
                record.command={opcode=queue.value,content_id=queue.content_id,start=true,admission='FIFO validation'}
            end
            assert(record.candidate.value==d[2] and record.candidate.size==d[3],'response candidate mismatch')
            assert(record.native_result==(phase==1 or phase==3),'native enqueue result differs from branch')
            if phase==0 then assert(record.before.head==record.after.head and record.before.tail==record.after.tail,'drop changed queue identity')
            elseif phase==1 then assert(record.after.head==record.candidate.content_id and record.after.tail==record.before.tail,'head insertion mismatch')
            elseif phase==2 then assert(record.after.tail==record.candidate.content_id and record.after.head==record.before.head,'tail insertion mismatch')
            else assert(record.after.head==record.before.tail and record.after.tail==record.candidate.content_id,'promotion insertion mismatch') end
            result.responses[#result.responses+1]=record
        elseif kind==39 then
            local record=M.reference(row);record.variant=phase;record.raw=m.array(d)
            if phase==1 then assert(d[3]<=1,'invalid IRQ assertion predicate')
            elseif phase==4 then assert(#payload==32,'invalid SPU handoff') end
            result.irqs[#result.irqs+1]=record
        end
    end
    function tracker.current() return stack[#stack] end
    function tracker.root() return stack[1] end
    function tracker.closed() return closed end
    function tracker.context() return last end
    function tracker.initial() return first end
    function tracker.environment() return environment end
    function tracker.finish()
        assert(count==status.events and offset==status.bytes and first and final and environment and #stack==0,'incomplete wire-3 controller boundaries')
        result.environment=environment;result.initial=first;result.final=final
        result.scope='wire-3 native scopes, four qualified queues, handler returns and response branches'
        return result
    end
    return tracker
end
return M
-- )EOF"
