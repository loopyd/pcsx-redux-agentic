-- lualoader, R"EOF(--
local ffi,c,m = require 'ffi',require 'controller',require 'support'
local M={}
local settings={'volume','interpolation','key_on_delay','mute','mono','streaming','null_sync','schema_version','reserved','speed',
    'forced_irq','voice_mute','voice_solo','xa'}
function M.new(controller,audio)
    local pending,environment,native,joined,epoch;local seen,joins={},{}
    local result={feeds=m.array()};local tracker={}
    local function verifyAudio(context,row)
        joined=context.audio_epoch~='0';epoch=context.audio_epoch
        assert(joined==(audio~=nil),'Audio export differs from native CD profile')
        if not joined then return end
        native=assert(audio.report and audio.report.native,'missing joined native Audio report')
        assert(audio.report.complete and type(audio.events)=='function' and native.version==2 and native.state==2 and
            native.failures==0 and native.dropped_events==0 and native.dropped_frames=='0' and native.streaming_dropped=='0' and
            native.epoch==epoch and m.uint64(native.begin_cycle)<=row.cycle,'joined Audio lifecycle mismatch')
        for i,name in ipairs(settings) do assert(ffi.cast('int64_t',m.u64le(environment,i*8-8))==native.settings[name],
            'joined native Audio setting mismatch: '..name) end
    end
    function tracker.push(row,payload)
        local kind,phase,d=tonumber(row.kind),tonumber(row.device),c.data(row)
        if kind==34 and phase==2 then
            assert(not environment and #payload==156+d[5]+d[6] and m.u64le(payload,56)==2 and m.u64le(payload,64)==0,
                'invalid joined environment');environment=payload
            if audio then
                local value=assert(audio.report and audio.report.native,'missing joined Audio report')
                assert(value.settings.configured_backend==payload:sub(157,156+d[5]) and
                    value.settings.configured_device==payload:sub(157+d[5],156+d[5]+d[6]),'joined Audio device selection differs')
            end
        elseif kind==34 and phase==0 then verifyAudio(c.context(payload),row)
        elseif kind==34 and phase==1 then
            assert(not pending and c.context(payload).audio_epoch==epoch,'unfinished feed or changed Audio epoch')
            if joined then assert(m.uint64(native.end_cycle)>=row.cycle,'Audio ended before native CD capture') end
        elseif kind==43 and phase==0 then
            local frame=c.owner(controller,row);local id=m.exact(row.request)
            assert(not pending and not seen[id] and #payload==24 and d[1]<=1 and d[4]<=1 and
                frame.role==(d[1]==1 and 7 or 6),'invalid native feed entry')
            if d[1]==1 then assert(d[2]==44100 and d[3]==588 and d[4]==1,'native CDDA source metadata differs') end
            seen[id]=true;pending=c.reference(row);pending.id=id;pending.access=frame.id;pending.cdda=d[1]~=0
            pending.input=m.array(d);pending.media=m.exact(m.u64le(payload,0));pending.mutation=m.exact(m.u64le(payload,8));pending.stream=m.exact(m.u64le(payload,16))
        elseif kind==43 and phase==1 then
            assert(pending and pending.id==m.exact(row.request) and #payload==16 and
                pending.access==m.exact(m.u64le(payload,8)) and pending.cdda==(d[8]~=0) and d[1]>=1 and d[1]<=7,
                'native feed outcome lacks matching input')
            assert(d[6]<=2 and d[7]<=3 and d[4]<=1,'invalid native feed disposition')
            if d[1]<=4 then assert(d[2]==0 and d[3]==0 and d[4]==0 and d[5]==0,'early native feed inspected source metadata')
            else assert(d[2]==pending.input[2] and d[3]==pending.input[3] and d[4]==pending.input[4] and
                ((d[1]==5)==(d[5]==0)),'native feed output metadata differs from inspected input') end
            pending.outcome=c.reference(row);pending.outcome.raw=m.array(d);pending.outcome.audio_epoch=m.exact(m.u64le(payload,0))
            if joined then
                assert(d[6]==1 and pending.outcome.audio_epoch==epoch,'native joined feed changed Audio lifecycle')
                if d[1]>=6 then
                    local id=m.exact(row.related);assert(d[7]==2 and id~='0' and not joins[id] and row.related<=native.events,
                        'joined feed lacks unique retained Audio event')
                    local event=audio.events(tonumber(row.related)-1,1)[1]
                    assert(event.id==tonumber(row.related) and event.kind==3 and event.parent=='0' and m.uint64(event.cycle)==row.cycle and
                        event.source_rate==d[2] and event.source_frames==d[3] and event.stereo==d[4] and
                        event.output_frames==d[5] and event.accepted==(d[1]==6 and 1 or 0) and event.cdda==d[8],
                        'native CD/Audio feed receipt mismatch')
                    joins[id]=true;pending.audio_event=id
                else assert(row.related==0,'unsubmitted native feed invented Audio event') end
            end
            result.feeds[#result.feeds+1]=pending;pending=nil
        end
    end
    function tracker.finish() assert(environment and joined~=nil and not pending,'incomplete native feed boundaries');
        result.joined=joined;result.audio_epoch=epoch;result.scope='actual native feed output counts and forward Audio joins; zigzag, ADPCM and audible fidelity remain separate';return result end
    return tracker
end
return M
-- )EOF"
