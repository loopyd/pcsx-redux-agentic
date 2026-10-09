-- lualoader, R"EOF(--
local m,c=require 'support',require 'controller'
local M={}
function M.new(limits)
    local first,final;local mutations={};local count,bytes=0,0
    local tracker={};local result={mutations=m.array(),consumptions=m.array()}
    function tracker.push(row,payload)
        local kind,phase,d=tonumber(row.kind),tonumber(row.device),c.data(row)
        if kind==41 then
            if phase<=1 then
                assert(d[1]==1 and #payload==2352,'invalid native FIFO boundary')
                if phase==0 then assert(not first,'duplicate initial FIFO');first=#payload
                else assert(first and not final,'invalid final FIFO boundary');final=#payload end
            else
                assert(first and not final and #payload==d[3],'mutation outside native FIFO boundaries')
                local id=m.exact(row.request);assert(not mutations[id],'duplicate native buffer identity')
                mutations[id]={buffer=d[1],offset=d[2],length=d[3]};count=count+1;bytes=bytes+#payload
                assert(count<=limits.mutations and bytes<=limits.bytes,'native retained mutation budget exceeded')
                if d[1]==1 then assert(d[2]+#payload<=2352,'FIFO mutation exceeds storage') end
                local record=c.reference(row);record.id=id;record.buffer=d[1];record.offset=d[2];record.bytes=#payload
                result.mutations[#result.mutations+1]=record
            end
        elseif kind==42 and phase<=1 then
            assert(first and not final and #payload==32+d[4]*16+d[2] and m.u32le(payload,0)==3 and m.u32le(payload,4)==phase and
                m.u32le(payload,8)==d[1] and m.u32le(payload,12)==d[2] and m.u32le(payload,16)==d[3] and m.u32le(payload,20)==d[4],
                'native consumed span descriptor mismatch')
            local cursor=0;local runs=m.array()
            for i=0,d[4]-1 do
                local offset,length=m.u32le(payload,32+i*16),m.u32le(payload,36+i*16);local mutation=m.exact(m.u64le(payload,40+i*16))
                assert(offset==cursor and length>0 and length<=d[2]-cursor,'noncanonical native origin runs')
                assert(mutation=='0' or mutations[mutation] and mutations[mutation].buffer==1,'native origin references unknown FIFO mutation')
                runs[#runs+1]={offset=offset,bytes=length,mutation=mutation,prehistory=mutation=='0'};cursor=cursor+length
            end
            assert(cursor==d[2],'native origin runs do not cover consumed bytes')
            local record=c.reference(row);record.fifo_offset=d[1];record.bytes=d[2];record.address=d[3];record.dma=phase==1;record.origins=runs
            result.consumptions[#result.consumptions+1]=record
        end
    end
    function tracker.mutation(id) return mutations[m.exact(id)] end
    function tracker.finish()
        assert(first and final,'missing native FIFO boundary');result.initial_bytes=first;result.final_bytes=final
        result.scope='native-verified FIFO shadows, consumed spans and origin runs; Lua validates bounds and formats owning payload references';return result
    end
    return tracker
end
return M
-- )EOF"
