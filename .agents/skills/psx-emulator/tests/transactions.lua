local f,transactions,m=require 'responses',require 'transactions',require 'support'
local j=f.journal();local limits={mutations=16,bytes=8388608}
f.identity(function()
    local result=transactions.scan(j.h,limits,nil,{})
    assert(result.complete and result.schema=='psx.runtime-cd-transactions/v2' and result.cd_observation_version==3)
    assert(result.events==#j.rows and result.payload_bytes==#j.payload and result.controller.initial.version==3)
    local reports={};local write=m.write;m.write=function(name,bytes) assert(name=='transactions.json');reports[#reports+1]=bytes end
    local native={schema='psx.native-history/v2',native_envelope_version=1,cd_observation_version=3,build_identity=m.buildIdentity(),complete=true,failure_bits=0,dropped_attempts=0,events=#j.rows,payload_bytes=#j.payload,begin_cycle=m.exact(j.cycle),end_cycle=m.exact(j.cycle)}
    assert(transactions.export(j.h,limits,native,nil,{}).complete)
    native.build_identity.binary_sha256=string.rep('c',64);assert(not transactions.export(j.h,limits,native,nil,{}).complete)
    native.build_identity=m.buildIdentity();native.complete=false;assert(not transactions.export(j.h,limits,native,nil,{}).complete)
    native.complete=true;native.events=native.events+1;assert(not transactions.export(j.h,limits,native,nil,{}).complete)
    assert(#reports==4 and reports[2]:find('complete":false',1,true));m.write=write
end)
print('revisioned full consumer composition, exact build/journal joins and retained incomplete export checks passed')
