local f,feeds=require 'responses',require 'feeds'
local function fixture(reason,frames)
    return f.journal(function(j) f.scope(j,7,0,0,function(owner)
        j.emit(43,0,{1,44100,588,1},10,owner,f.u64(7)..f.u64(8)..f.u64(9))
        j.emit(43,1,{reason,reason<=4 and 0 or 44100,reason<=4 and 0 or 588,reason<=4 and 0 or 1,frames,0,reason>=6 and 1 or 0,1},10,0,f.u64(0)..f.u64(owner))
    end) end)
end
for _,pair in ipairs({{4,0},{5,0},{6,591},{7,585}}) do
    local r=f.scan(fixture(pair[1],pair[2]),function(c) return feeds.new(c) end)
    assert(r.feeds[1].outcome.raw[5]==pair[2] and not r.joined)
end
f.reject(function() f.scan(fixture(4,1),function(c) return feeds.new(c) end) end,'inspected')
f.reject(function() f.scan(fixture(5,3),function(c) return feeds.new(c) end) end,'output metadata')
print('native observed zigzag output counts, closed/zero/submitted feed dispositions and early-read refusal passed')
