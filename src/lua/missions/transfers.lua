-- lualoader, R"EOF(--
local m,c=require 'support',require 'controller'
local M={}
function M.new(controller)
    local tracker={};local pending;local result={transfers=m.array(),copied_bytes=0,dropped_bytes=0}
    function tracker.push(row,payload)
        if row.kind~=42 then return end
        local phase,d=tonumber(row.device),c.data(row)
        if phase==1 then assert(not pending,'overlapping native CD DMA');pending={record=c.reference(row),address=d[3],fifo_offset=d[1],requested=d[2],receipts=m.array(),copied=0,dropped=0,receipted=0}
        elseif phase==3 then
            assert(pending and d[1]==(pending.address+pending.receipted)%4294967296 and d[2]==pending.fifo_offset+pending.receipted and
                d[3]==d[4]+d[5] and pending.receipted+d[3]<=pending.requested,'native DMA receipt range mismatch')
            pending.receipts[#pending.receipts+1]={address=d[1],fifo_offset=d[2],requested=d[3],copied=d[4],dropped=d[5],destination=d[6],destination_offset=d[7]}
            pending.copied=pending.copied+d[4];pending.dropped=pending.dropped+d[5];pending.receipted=pending.receipted+d[3]
        elseif phase==4 then
            assert(pending and d[1]==pending.address and d[2]==pending.fifo_offset and d[3]==pending.requested and d[4]==pending.receipted and d[5]==pending.receipted,
                'native returned DMA count lacks complete copied/dropped receipts')
            pending.returned=d[4];result.copied_bytes=result.copied_bytes+pending.copied;result.dropped_bytes=result.dropped_bytes+pending.dropped
            result.transfers[#result.transfers+1]=pending;pending=nil
        end
    end
    function tracker.finish() assert(not pending,'unfinished native DMA receipts');result.scope='actual mapped copies, scratchpad prefix and discarded destination bytes; returned count alone is not delivery';return result end
    return tracker
end
return M
-- )EOF"
