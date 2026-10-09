-- lualoader, R"EOF(--
local c,m,bit = require 'controller',require 'support',require 'bit'
local M={}
function M.new(controller)
    local expected={};local result={decisions=m.array(),decodes=m.array()};local tracker={}
    function tracker.push(row,payload)
        if tonumber(row.kind)~=46 then return end
        local phase,d=tonumber(row.device),c.data(row)
        if phase~=12 and phase~=13 then return end
        local frame=assert(controller.current(),'native XA decision lacks scope');assert(frame.role==6,'native XA decision outside read')
        local record=c.reference(row);record.access=frame.id;record.raw=m.array(d)
        if phase==12 then
            assert(d[1]>=1 and d[1]<=5 and d[2]<=1,'unknown native XA branch site')
            local site=d[1];record.site=site;record.branch=d[2]~=0
            if site==2 then assert(record.branch==(d[3]~=0 and (d[4]~=d[6] or d[5]~=d[7])),'native XA filter receipt mismatch')
            elseif site==3 then assert(record.branch==(d[3]==255),'native XA channel receipt mismatch')
            elseif site==4 then assert(record.branch==(d[3]==0 and d[4]==0),'native XA mute receipt mismatch')
            elseif site==5 then assert(record.branch==(d[3]~=0 and d[4]~=0),'native XA EOF/EOR receipt mismatch') end
            local previous=expected[frame.id]
            if site<=3 then
                assert(site==(previous or 1),'native XA short-circuit branch order mismatch')
                if record.branch then expected[frame.id]=nil else expected[frame.id]=site+1 end
            elseif site==4 then assert(previous==4,'native XA mute gate preceded decoder result');expected[frame.id]=5
            elseif site==5 then assert(previous==5,'native XA termination gate order mismatch');expected[frame.id]=nil end
            result.decisions[#result.decisions+1]=record
        else
            assert(d[6]==1 or d[6]==2,'native XA decode lacks site discriminator')
            record.site=d[6]
            if d[6]==1 then assert(expected[frame.id]==4,'native XA decode preceded three entry gates');record.native_result=d[1];record.first_sector=d[2]~=0
                expected[frame.id]=d[1]==0 and 4 or 5
            else assert(expected[frame.id]==4 and d[1]==0 and d[4]<=1,'invalid native XA format receipt')
                record.rate=d[2];record.bits=d[3];record.stereo=d[4];record.frames=d[5] end
            result.decodes[#result.decodes+1]=record
        end
    end
    function tracker.finish() assert(not next(expected),'unfinished native XA branch lineage');result.scope='observed XA entry/filter/channel/mute/end guards and decoder result/format; no DSP arithmetic claim';return result end
    return tracker
end
return M
-- )EOF"
