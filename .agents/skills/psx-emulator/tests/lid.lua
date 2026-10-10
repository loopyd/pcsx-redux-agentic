local f,lid=require 'responses',require 'lid'
local j=f.journal(function(j) f.scope(j,8,2,0,function(owner) j.emit(46,15,{1,1,4294967295},0,owner) end,
    {flags=1},{flags=5,close_cycle=4294967295}) end)
local r=f.scan(j,function(c) return lid.new(c) end);assert(#r.accesses==1 and r.decisions[1].guest_close_cycle==4294967295)
local bad=f.journal(function(j) f.scope(j,8,0,0,function(owner) j.emit(46,15,{2,0,0},0,owner) end) end)
f.reject(function() f.scan(bad,function(c) return lid.new(c) end) end,'invalid native lid')
print('native lid state/deadline receipts and invalid boolean refusal passed')
