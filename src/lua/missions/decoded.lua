-- lualoader, R"EOF(--
local c,m,bit = require 'controller',require 'support',require 'bit'
local M={}
local function status(bytes)
    assert(#bytes==48,'invalid native worker status size')
    return {epoch=m.exact(m.u64le(bytes,0)),begin_batch=m.exact(m.u64le(bytes,8)),end_batch=m.exact(m.u64le(bytes,16)),
        version=m.u32le(bytes,24),state=m.u32le(bytes,28),events=m.u32le(bytes,32),limit=m.u32le(bytes,36),failures=m.u32le(bytes,40),dropped=m.u32le(bytes,44)}
end
function M.new(controller,h)
    local first,final;local result={records=m.array(),handoffs=m.array(),unconsumed_publications=m.array()};local tracker={}
    function tracker.push(row,payload)
        local kind,phase=tonumber(row.kind),tonumber(row.device)
        if kind==45 and phase>=16 then
            local value=status(payload);assert(value.version==3 and value.failures==0 and value.dropped==0 and value.limit>=1 and value.limit<=65536,
                'incomplete native worker boundary')
            if phase==16 then assert(not first and value.state==1 and value.events==0,'invalid native worker start');first=value
            else assert(phase==17 and first and not final and value.state==2 and value.epoch==first.epoch and
                value.limit==first.limit and value.events<=value.limit,'invalid native worker freeze');final=value end
        elseif kind==39 and phase==4 then
            assert(first and not final and #payload==32,'SPU handoff outside worker capture')
            local record=c.reference(row);record.epoch=m.exact(m.u64le(payload,0));record.first=m.exact(m.u64le(payload,8));record.last=m.exact(m.u64le(payload,16))
            record.count=m.u32le(payload,24);record.flags=m.u32le(payload,28)
            assert(record.epoch==first.epoch and record.count>=1 and record.flags<=4 and
                bit.band(record.flags,3)==0 and (bit.band(record.flags,4)~=0)==(record.count>1),'unqualified SPU producer ancestry')
            result.handoffs[#result.handoffs+1]=record
        end
    end
    function tracker.finish()
        if not first then assert(not final and #result.handoffs==0,'worker evidence without profile');result.enabled=false;return result end
        assert(final and type(h.cdWorkerRecords)=='function','worker lane not frozen')
        local native=h.cdWorkerStatus()
        assert(native.version==3 and native.state==2 and native.failures==0 and native.dropped==0 and native.epoch==m.uint64(final.epoch) and
            native.events==final.events and native.limit==final.limit and native.beginBatch==m.uint64(final.begin_batch) and native.endBatch==m.uint64(final.end_batch),
            'native worker status differs from CPU freeze receipt')
        local batch,open,previous,producers=nil,false,nil,{}
        local records=h.cdWorkerRecords(0,final.events)
        for index=0,final.events-1 do
            local row=records[index];local kind,flags=tonumber(row.kind),tonumber(row.flags);local data={}
            for n=0,5 do data[n+1]=tonumber(row.data[n]) end
            assert(row.sequence==index+1 and row.epoch==native.epoch and kind>=1 and kind<=6 and flags<=1 and
                (not previous or row.cpuPublicationCycle>=previous),'invalid native worker chronology')
            previous=row.cpuPublicationCycle
            local id=m.exact(row.batch);local record={sequence=index+1,epoch=final.epoch,batch=id,sample_ordinal=m.exact(row.sampleOrdinal),
                last_published_cpu_cycle=m.exact(row.cpuPublicationCycle),last_published_feed=m.exact(row.relatedFeed),kind=kind,flags=flags,raw=m.array(data)}
            if kind==1 then assert(not open and flags==0 and (not batch or row.batch>m.uint64(batch)),'nested/nonmonotonic worker batch');batch=id;open=true
            else assert(open and batch==id,'worker observation outside complete batch') end
            if kind==6 then open=false
            elseif kind==2 then
                record.condition_evaluated=flags==1
                assert(data[4]<=1 and data[5]<=1,'invalid decoded-buffer predicate')
                if flags==1 then assert(data[6]>=1 and (data[5]~=0)==(bit.band(data[3],64)~=0 and data[2]~=4294967295 and data[2]<4096),
                    'decoded-buffer native eligibility predicate mismatch') end
            elseif kind==3 then
                assert(data[1]<4 and data[5]<=1 and (data[5]~=0)==(data[3]>=data[2] and data[3]<data[2]+2),
                    'decoded-buffer native region predicate mismatch')
            elseif kind==4 then
                record.address_conditions_evaluated=bit.band(data[3],64)~=0
                assert(data[1]<24 and data[5]<=1 and data[6]<=1,'invalid native voice IRQ operands')
                if not record.address_conditions_evaluated then assert(data[5]==0 and data[6]==0,'disabled voice IRQ invented address predicates') end
            elseif kind==5 then assert(flags==0 and data[5]<=1,'invalid atomic SPU publication');producers[index+1]=record end
            result.records[#result.records+1]=record
        end
        assert(not open and (final.events==0 or result.records[1].batch==final.begin_batch and batch==final.end_batch),'native worker batches incomplete')
        local used,last={},0
        for _,handoff in ipairs(result.handoffs) do
            local first,lastSequence=tonumber(m.uint64(handoff.first)),tonumber(m.uint64(handoff.last));local count=0
            assert(first>last and lastSequence>=first and lastSequence<=final.events,'SPU handoff sequence range overlaps or exceeds lane')
            for sequence=first,lastSequence do
                local producer=producers[sequence]
                if producer then assert(not used[sequence],'SPU publication consumed twice');used[sequence]=true;count=count+1 end
            end
            assert(producers[first] and producers[lastSequence] and count==handoff.count,'SPU handoff does not cover exact atomic publications');last=lastSequence
        end
        for sequence in pairs(producers) do if not used[sequence] then result.unconsumed_publications[#result.unconsumed_publications+1]=sequence end end
        table.sort(result.unconsumed_publications);result.enabled=true;result.status=final
        result.scope='separate native worker chronology, qualified predicates and atomic CPU handoffs; published CPU cycle is not worker execution time; no DSP or deadlock/performance claim'
        return result
    end
    return tracker
end
return M
-- )EOF"
