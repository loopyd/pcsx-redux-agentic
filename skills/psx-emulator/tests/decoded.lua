local f,decoded,ffi=require 'responses',require 'decoded',require 'ffi'
local rows={}
local function worker(kind,data,flags)
    local n=#rows+1;local values={};for i=0,5 do values[i]=(data or {})[i+1] or 0 end
    rows[n]={sequence=n,epoch=ffi.new('uint64_t',7),batch=ffi.new('uint64_t',1),sampleOrdinal=ffi.new('uint64_t',n),cpuPublicationCycle=ffi.new('uint64_t',9007199254740992)+1,relatedFeed=ffi.new('uint64_t',0),kind=kind,flags=flags or 0,data=values}
    return rows[n]
end
worker(1)
local predicate=worker(2,{0,0,64,0,1,1},1)
worker(5,{0,0,0,0,0,0})
worker(6)
local function status(state,count)
    return f.u64(7)..f.u64(state==1 and 0 or 1)..f.u64(state==1 and 0 or 1)..f.u32(3)..f.u32(state)..f.u32(count)..f.u32(16)..f.u32(0)..f.u32(0)
end
local j=f.journal(function(j)
    j.emit(45,16,nil,nil,nil,status(1,0))
    j.emit(39,4,nil,nil,nil,f.u64(7)..f.u64(3)..f.u64(3)..f.u32(1)..f.u32(0))
    j.emit(45,17,nil,nil,nil,status(2,4))
end)
j.h.cdWorkerStatus=function() return {epoch=ffi.new('uint64_t',7),beginBatch=ffi.new('uint64_t',1),endBatch=ffi.new('uint64_t',1),version=3,state=2,events=4,limit=16,failures=0,dropped=0} end
j.h.cdWorkerRecord=function(index) return assert(rows[index+1]) end
local function scan() return f.scan(j,function(c) return decoded.new(c,j.h) end) end
local result=scan();assert(result.enabled and #result.records==4 and #result.handoffs==1 and #result.unconsumed_publications==0)
predicate.data[4]=0;f.reject(scan,'decoded-buffer native eligibility predicate mismatch');predicate.data[4]=1
rows[4].batch=ffi.new('uint64_t',2);f.reject(scan,'worker observation outside complete batch');rows[4].batch=ffi.new('uint64_t',1)
rows[3].cpuPublicationCycle=ffi.new('uint64_t',0);f.reject(scan,'invalid native worker chronology');rows[3].cpuPublicationCycle=predicate.cpuPublicationCycle
j.h.cdWorkerStatus=function() return {version=3,state=1} end;f.reject(scan,'native worker status differs')
print('separate worker batches, decoded predicate operands, exact publication handoff and refusal checks passed')
