-- lualoader, R"EOF(--
local m,events,image,c = require 'support',require 'events',require 'image',require 'controller'
local M={}
local function survivingWork(result)
    for _,transition in ipairs(result.streams.transitions) do
        for _,pending in ipairs(transition.pending_schedules or {}) do
            pending.outcome='unresolved'
            for _,record in ipairs(result.scheduler.records) do
                if record.slot==pending.slot and tonumber(record.sequence)>tonumber(transition.record.sequence) then
                    local changed=(record.variant==1 or record.variant==2) and record.previous_owner==pending.owner and record.old_target==pending.target
                    local dispatched=record.variant==5 and record.previous_owner==pending.owner and record.target==pending.target
                    local retained=record.variant==7 and record.owner==pending.owner and record.target==pending.target and record.pending
                    if changed or dispatched or retained then
                        pending.outcome=dispatched and 'dispatched' or retained and 'pending-at-end' or record.variant==2 and 'cancelled' or 'replaced'
                        pending.outcome_record=record
                        break
                    end
                end
            end
        end
    end
end
local function referenceContexts(result)
    local function context(record)
        local ref=assert(record.payload)
        assert(ref.file=='payload.bin' and ref.bytes==384 and ref.offset>=0 and ref.offset%1==0 and
            ref.offset+ref.bytes<=result.payload_bytes,'invalid owning CD context reference')
        return ref
    end
    local controller=result.controller
    for _,frame in ipairs(controller.accesses) do
        frame.before=context(frame.enter)
        frame.after=context(frame.exit)
    end
    local boundaries={}
    for _,boundary in ipairs(controller.contexts) do
        boundary.state=context(boundary.record)
        boundaries[boundary.phase]=boundary.state
    end
    controller.initial=assert(boundaries[0])
    controller.final=assert(boundaries[1])
    result.context_encoding='wire-3-payload-reference/384'
end
function M.scan(h,limits,audio,bindings,correlated)
    if type(h.snapshot)=='function' then h=h.snapshot() end
    local status=h.status();local controller=c.new(status);local sectors=require('sectors').new(limits)
    correlated=correlated or (type(h.correlateCD)=='function' and h.correlateCD())
    assert(type(correlated)=='table' and type(correlated.decoded)=='table' and type(correlated.feeds)=='table',
        'native CD correlation or an owning correlated report is required')
    local names={'controller','scheduler','streams','media','sectors','audio','eligibility','delivery','transfers','command',
        'submission','playback','lid'}
    local consumers={controller,require('scheduler').new(controller),require('streams').new(controller),
        require('media').new(controller),sectors,require('feeds').new(controller,audio,correlated.feeds),require('eligibility').new(controller),
        require('delivery').new(controller),require('transfers').new(controller),require('command').new(controller),
        require('submission').new(controller),require('playback').new(controller),require('lid').new(controller)}
    local images,geometry,previous,boundaries={}
    local records=h.records(0,tonumber(status.events))
    local frozen=h.bytes(0,tonumber(status.bytes))
    for index=0,tonumber(status.events)-1 do
        local row=records[index];events.describe(row)
        local offset,length=tonumber(row.offset),tonumber(row.length)
        assert(offset>=0 and length>=0 and offset%1==0 and length%1==0 and offset+length<=tonumber(status.bytes),
            'native CD payload exceeds frozen journal')
        local payload=frozen:sub(offset+1,offset+length);assert(#payload==length,'short owning native payload copy')
        if row.kind==47 then
            local phase=tonumber(row.device);assert(not controller.current() and not images[phase],'nested or duplicate image boundary')
            local decoded=image.decode(row,payload)
            if phase==0 then assert(previous and previous.kind==34 and previous.device==2,'initial native image lacks environment');geometry=payload
            else assert(images[0] and geometry==payload,'native mounted image changed across capture') end
            images[phase]={descriptor=decoded,payload=c.reference(row).payload,binding=image.bind(decoded,bindings)}
        elseif row.kind==34 and row.device==0 then assert(images[0] and not images[1],'native controller starts without image')
        elseif row.kind==34 and row.device==1 then assert(images[1] and previous and previous.kind==47 and previous.device==1,
            'native controller ends without adjacent final image')
        elseif images[1] then assert((row.kind==41 and row.device==1) or (row.kind==37 and row.device==7),
            'unexpected activity after final native image') end
        for _,consumer in ipairs(consumers) do consumer.push(row,payload) end
        if row.kind==41 and row.device<=1 then
            boundaries=boundaries or {};boundaries[tonumber(row.device)]=c.reference(row).payload
        end
        previous=row
    end
    assert(images[0] and images[1],'native image boundaries incomplete')
    local result={schema='psx.runtime-cd-transactions/v3',native_envelope_version=1,cd_observation_version=3,
        build_identity=m.buildIdentity(),complete=true,begin_cycle=m.exact(status.beginCycle),end_cycle=m.exact(status.endCycle),
        events=tonumber(status.events),payload_bytes=tonumber(status.bytes),image={initial=images[0],final=images[1]},decoded=correlated.decoded}
    for i,consumer in ipairs(consumers) do result[names[i]]=consumer.finish() end
    survivingWork(result)
    assert(boundaries and boundaries[0] and boundaries[1] and result.sectors.initial_bytes==boundaries[0].bytes and
        result.sectors.final_bytes==boundaries[1].bytes,'native FIFO export boundaries mismatch')
    result.sectors.initial,result.sectors.final=boundaries[0],boundaries[1]
    result.limits={mutations=limits.mutations,bytes=limits.bytes}
    result.scope='wire-3 source-native contexts, queues, scheduler, media, consumed spans, feed joins and separately frozen worker lane'
    result.exclusions=m.array({'old wire-2 fixtures and removed callback meanings','hardware/CD conformance','DSP arithmetic and audible fidelity',
        'per-byte bus timing','worker execution timestamps','worker race/deadlock/performance qualification'})
    referenceContexts(result)
    return result
end
function M.export(h,limits,native,audio,bindings,snapshot_error,prefix)
    prefix=prefix or ''
    assert(type(prefix)=='string' and #prefix<=32 and prefix:match('^[%w_.-]*$'),'invalid capture prefix')
    local ok,result,encoded=pcall(function()
        assert(not snapshot_error,'native CD snapshot unavailable: '..tostring(snapshot_error))
        local status=h.status();local build_identity=m.buildIdentity()
        assert(native and native.schema=='psx.native-history/v2' and native.native_envelope_version==1 and native.cd_observation_version==3 and
            native.build_identity.binary_sha256==build_identity.binary_sha256 and native.build_identity.source_manifest_sha256==build_identity.source_manifest_sha256 and
            native.complete and native.failure_bits==0 and native.dropped_attempts==0 and native.events==tonumber(status.events) and
            native.payload_bytes==tonumber(status.bytes) and native.begin_cycle==m.exact(status.beginCycle) and native.end_cycle==m.exact(status.endCycle),
            'native CD correlation requires matching complete revisioned journal and exact build receipt')
        local value=M.scan(h,limits,audio,bindings)
        if prefix~='' then
            local function files(node)
                if type(node)~='table' then return end
                for key,item in pairs(node) do
                    if key=='file' and item=='payload.bin' then node[key]=prefix..item
                    elseif type(item)=='table' then files(item) end
                end
            end
            files(value)
        end
        local bytes=m.json(value);assert(#bytes<=67108864,'native correlation export exceeds 64 MiB')
        return value,bytes
    end)
    if not ok then result={schema='psx.runtime-cd-transactions/v3',native_envelope_version=1,cd_observation_version=3,complete=false,error=tostring(result)};
        encoded=m.json(result) end
    m.write(prefix..'transactions.json',encoded..'\n');return result
end
return M
-- )EOF"
