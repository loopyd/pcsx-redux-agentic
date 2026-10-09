-- lualoader, R"EOF(--
local m,c=require 'support',require 'controller'
local M={}
function M.new(controller,scheduler)
    local previous;local result={transitions=m.array(),feeds=m.array()};local tracker={}
    function tracker.push(row,payload)
        local kind,phase=tonumber(row.kind),tonumber(row.device)
        if kind==34 and phase<=1 or kind==35 or kind==45 and phase<=1 then
            local value=c.context(payload)
            if previous and (previous.reading_type~=value.reading_type or previous.reading_state~=value.reading_state or previous.status~=value.status or previous.identities.stream~=value.identities.stream) then
                result.transitions[#result.transitions+1]={record=c.reference(row),before={type=previous.reading_type,state=previous.reading_state,status=previous.status,stream=previous.identities.stream},
                    after={type=value.reading_type,state=value.reading_state,status=value.status,stream=value.identities.stream}}
            end
            previous=value
        elseif kind==43 and phase==0 then
            result.feeds[#result.feeds+1]={record=c.reference(row),media=m.exact(m.u64le(payload,0)),mutation=m.exact(m.u64le(payload,8)),stream=m.exact(m.u64le(payload,16))}
        end
    end
    function tracker.finish() result.scope='unified native Normal/Streaming/Play state and actual feed ancestry; callback snapshots bound transition timing';return result end
    return tracker
end
return M
-- )EOF"
