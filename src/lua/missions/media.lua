-- lualoader, R"EOF(--
local c,m,ffi = require 'controller',require 'support',require 'ffi'
local M={}
function M.new(controller)
    local pending,seen=nil,{};local result={lookups=m.array(),queries=m.array(),iso_scans=m.array()};local tracker={}
    function tracker.push(row,payload)
        if tonumber(row.kind)~=40 then return end
        local phase,d=tonumber(row.device),c.data(row)
        local record=c.reference(row);record.raw=m.array(d)
        if phase==0 then
            local owner=c.owner(controller,row);local id=m.exact(row.request)
            assert(not pending and not seen[id] and id~='0' and d[1]>=1 and d[1]<=3,'invalid native lookup entry')
            seen[id]=true;record.id=id;record.access=owner.id;record.kind=d[1];record.packed_msf=d[2];record.absolute_frame=d[3]
            record.prior_backing=m.exact(ffi.new('uint64_t',d[5])*4294967296+d[4]);pending=record
        elseif phase==2 then
            c.owner(controller,row)
            assert(pending and pending.id==m.exact(row.request) and not pending.outcome,'unpaired native backend outcome')
            record.reason=d[1];record.native_count=tostring(ffi.cast('int64_t',ffi.new('uint64_t',d[3])*4294967296+d[2])):gsub('LL$','')
            record.track=d[4];record.file=d[5];record.sector_bits=d[6];record.flags=d[7];record.base_offset=d[8];pending.outcome=record
        elseif phase==1 then
            c.owner(controller,row)
            assert(pending and pending.id==m.exact(row.request) and d[1]==pending.kind and d[2]<=1 and d[3]<=2 and
                ((d[3]==0)==(pending.outcome~=nil)),'native lookup result lacks matching backend receipt')
            pending.success=d[2]~=0;pending.disposition=d[3];pending.finish=record
            result.lookups[#result.lookups+1]=pending;pending=nil
        elseif phase==6 then
            assert(controller.current(),'native query lacks scope')
            record.query=d[1];record.argument=d[2];record.native_result=d[3]
            if #payload~=0 then assert(d[1]==6 and d[3]==1 and #payload==8,'unqualified query payload');record.loc_p_hex=m.hex(payload) end
            result.queries[#result.queries+1]=record
        else
            assert((phase==4 or phase==5) and #payload==64,'invalid native ISO identity boundary')
            record.phase=phase;record.iso_id=payload:sub(1,d[1]);record.iso_label=payload:sub(33,32+d[2])
            assert(d[1]<=9 and d[2]<=32,'native ISO identity exceeds defined strings');result.iso_scans[#result.iso_scans+1]=record
        end
    end
    function tracker.finish()
        assert(not pending,'unfinished native media lookup');result.scope='actual native lookup/backend outcomes and getter returns; no cache, format or SubQ conformance prediction';return result
    end
    return tracker
end
return M
-- )EOF"
