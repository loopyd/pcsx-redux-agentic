-- lualoader, R"EOF(--
local c,m = require 'controller',require 'support'
local M={}
function M.new(controller)
    return c.collect(function(kind,phase) return kind==46 and phase==14 end,function(record,row,payload)
        local phase,d=tonumber(row.device),c.data(row)
        local frame=assert(controller.current(),'native playback branch lacks scope');assert(frame.role==7,'native playback branch outside play')
        assert(d[1]>=1 and d[1]<=6 and d[2]<=1,'unknown native playback site')
        record.access=frame.id;record.site=d[1];record.branch=d[2]~=0
        if d[1]==3 then assert(record.branch==(d[3]~=0 and d[4]~=d[5]),'native autopause predicate mismatch')
        elseif d[1]==5 then assert(record.branch==(d[3]~=0 and d[4]~=0 and d[5]~=0),'native report predicate mismatch')
        elseif d[1]==6 then assert(record.branch==(d[3]%10==0),'native report cadence mismatch') end
    end,function(result) result.scope='actual native end/autopause/mute/report decisions; no removed play callback or PCM peak reconstruction' end)
end
return M
-- )EOF"
