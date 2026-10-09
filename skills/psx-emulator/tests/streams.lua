local f,streams=require 'responses',require 'streams'
for readingType=0,3 do
    local j=f.journal(function(j) f.scope(j,6,0,0,nil,{reading_type=0},{reading_type=readingType,stream=readingType}) end)
    local r=f.scan(j,function(c) return streams.new(c) end)
    assert((readingType==0 and #r.transitions==0) or (readingType~=0 and #r.transitions>=1 and r.transitions[1].after.type==readingType))
end
local j=f.journal(function(j) f.scope(j,6,0,0,nil,{reading_type=4}) end)
f.reject(function() f.scan(j,function(c) return streams.new(c) end) end,'enumeration')
print('unified native four-value reading type transitions and unknown enum refusal passed')
