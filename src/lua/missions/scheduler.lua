-- lualoader, R"EOF(--
local ffi,bit,m,c=require 'ffi',require 'bit',require 'support',require 'controller'
local M={}
function M.new(controller)
    local tracker={};local owners,targets={},{};local boundaries={};local pass
    local result={records=m.array(),slots=m.array({2,3,4,10}),scale_bits=m.array(),passes=0}
    function tracker.push(row,payload)
        if row.kind==34 and row.device==2 then
            assert(tonumber(row.data[3])==11,'scheduler scale catalogue differs')
            for i=0,10 do result.scale_bits[i+1]=m.u32le(payload,112+i*4) end
        elseif row.kind==37 then
            local phase,d=tonumber(row.device),c.data(row);local record=c.reference(row);record.variant=phase
            if phase==3 then pass=m.exact(row.request);result.passes=result.passes+1;record.pass=pass;record.snapshot_mask=d[1];record.live_mask=d[2]
            elseif phase==6 or phase==7 then
                local slot=d[1];assert(slot==2 or slot==3 or slot==4 or slot==10,'unknown native CD slot')
                boundaries[phase]=boundaries[phase] or {};assert(not boundaries[phase][slot],'duplicate schedule boundary');boundaries[phase][slot]=true
                if phase==6 then owners[slot]=m.exact(row.request);targets[slot]=m.exact(row.related)
                else assert(owners[slot]==m.exact(row.request),'final native schedule ancestry differs') end
                record.slot=slot;record.pending=d[2]~=0;record.target=m.exact(row.related);record.owner=m.exact(row.request);record.scale_bits=d[5]
            else
                assert(#payload==48,'short native scheduler transition')
                local slot=d[1];local owner=m.exact(m.u64le(payload,8));local old=m.exact(m.u64le(payload,16));local target=m.exact(m.u64le(payload,24))
                assert(owner==owners[slot],'scheduler replaced wrong native owner')
                record.slot=slot;record.pass=m.exact(m.u64le(payload,0));record.previous_owner=owner;record.old_target=old;record.target=target
                record.snapshot_mask=m.u32le(payload,32);record.live_mask=m.u32le(payload,36);record.scale_bits=m.u32le(payload,40)
                assert(m.u32le(payload,44)==0 and record.scale_bits==result.scale_bits[slot+1],'scheduler reserved/scale mismatch')
                if phase==1 or phase==2 then owners[slot]=m.exact(row.request);targets[slot]=target
                    assert(d[2]==(phase==2 and 1 or 0),'schedule cancel branch mismatch')
                else
                    assert(pass and record.pass==pass and target==m.exact(old),'scheduler target-test pass mismatch')
                    if phase==5 then assert(d[2]==1,'dispatch predicate missing');owners[slot]='0' end
                end
            end
            result.records[#result.records+1]=record
        end
    end
    function tracker.owner(slot) return owners[slot] or '0' end
    function tracker.finish()
        for _,slot in ipairs(result.slots) do assert(boundaries[6] and boundaries[6][slot] and boundaries[7] and boundaries[7][slot],'missing CD schedule boundary') end
        result.scope='snapshot-pass mask, live tested targets, four CD owners and eleven exact scale patterns';return result
    end
    return tracker
end
return M
-- )EOF"
