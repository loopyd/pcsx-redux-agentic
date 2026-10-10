local f,playback=require 'responses',require 'playback'
local function fixture(value)
    return f.journal(function(j) f.scope(j,7,0,0,function(owner)
        j.emit(46,14,{1,0},0,owner);j.emit(46,14,{2,1},0,owner);j.emit(46,14,{3,value,1,2,1},0,owner)
        j.emit(46,14,{5,1,1,1,1,30000000},0,owner);j.emit(46,14,{6,1,10},0,owner)
    end) end)
end
local r=f.scan(fixture(1),function(c) return playback.new(c) end);assert(#r.records==5 and r.records[3].branch)
f.reject(function() f.scan(fixture(0),function(c) return playback.new(c) end) end,'autopause')
print('native end/Q/autopause/report branch receipts and inconsistency refusal passed')
