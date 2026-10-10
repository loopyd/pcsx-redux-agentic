local f,opcode=require 'responses',require 'opcode'
for op=0,30 do
    local record={opcode=op,start=true,queue={size=math.max(opcode.describe(op).arguments,0)}}
    if op==23 or op==24 then f.reject(function() opcode.verify(record) end)
    else assert(opcode.verify(record).opcode==op);if record.queue.size>=0 and opcode.describe(op).arguments>=0 then
        record.queue.size=record.queue.size+1;f.reject(function() opcode.verify(record) end) end end
end
assert(not opcode.describe(255).handler and opcode.describe(29).arguments==2)
f.reject(function() opcode.describe(0x106) end)
print('literal native opcode/argument catalogue and obsolete delayed-opcode refusal passed')
