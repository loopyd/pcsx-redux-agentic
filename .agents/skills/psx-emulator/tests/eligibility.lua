local f,eligibility=require 'responses',require 'eligibility'
local function fixture(filter)
    return f.journal(function(j) f.scope(j,6,0,0,function(owner)
        j.emit(46,12,{1,0},0,owner);j.emit(46,12,{2,filter and 1 or 0,1,1,2,filter and 9 or 1,2},0,owner)
        if not filter then j.emit(46,12,{3,0,2},0,owner);j.emit(46,13,{0,1,1,2,0,1},0,owner)
            j.emit(46,13,{0,37800,4,1,2016,2},0,owner);j.emit(46,12,{4,1,0,0},0,owner);j.emit(46,12,{5,1,1,1},0,owner) end
    end) end)
end
for _,filter in ipairs({true,false}) do local r=f.scan(fixture(filter),function(c) return eligibility.new(c) end);assert(#r.decodes==(filter and 0 or 2)) end
local bad=fixture(false);for _,row in ipairs(bad.rows) do if row.kind==46 and row.data[0]==2 then row.data[1]=1 end end
f.reject(function() f.scan(bad,function(c) return eligibility.new(c) end) end,'filter receipt')
print('native XA short-circuit filter/decode/mute/end chronology and inconsistent predicate refusal passed')
