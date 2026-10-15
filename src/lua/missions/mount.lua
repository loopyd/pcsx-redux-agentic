-- lualoader, R"EOF(--
-- Associate native absolute paths with literal harness staging identities.
-- Final input stability is established by the passed harness receipt, not Lua.
local m = require 'support'
local M = {}
local hex=m.hex

function M.parse(directory, text)
    assert(type(directory)=='string' and #directory>1 and #directory<=4096 and
        directory:sub(1,1)=='/' and directory:sub(-1)~='/' and not directory:find('%z') and
        not directory:find('//',1,true) and not (directory..'/'):find('/./',1,true) and
        not (directory..'/'):find('/../',1,true), 'missing or invalid harness runtime directory')
    assert(type(text)=='string' and #text<=122880 and (text=='' or text:sub(-1)=='\n'),
        'missing or invalid harness media identities')
    local entries,total,groups,groupCount=m.array(),0,{},0
    local previous
    for line in text:gmatch('([^\n]*)\n') do
        local key,size,hash=line:match('^([^\t]+)\t([1-9]%d*)\t([0-9a-f]+)$')
        assert(key and #size<=10 and #hash==64,'invalid harness media identity')
        local name,track=key:match('^replacement:([a-z][a-z0-9-]*):(track%d%d%.bin)$')
        local group,folder
        if name then
            assert(#name<=32,'invalid replacement name')
            group,folder='replacement:'..name,'replacements/'..name..'/disc'
        else
            track=key:match('^disc:(track%d%d%.bin)$')
            assert(track and groupCount==0 or track and groups.disc,'invalid harness media identity')
            group,folder='disc','disc'
        end
        if previous~=group then
            assert(not groups[group] and groupCount<8,'unordered or repeated harness disc group')
            groups[group]={count=0,bytes=0};groupCount=groupCount+1;previous=group
        end
        local current=groups[group]
        current.count=current.count+1
        local bytes=tonumber(size)
        total=total+bytes;current.bytes=current.bytes+bytes
        assert(current.count<=99 and track==string.format('track%02d.bin',current.count) and
            bytes<=1073741824 and current.bytes<=2147483648 and total<=8589934592,
            'harness media exceeds staging bounds or track order')
        local path=directory..'/'..folder..'/'..track
        assert(#path<=4096,'staged CD path exceeds native descriptor bound')
        entries[#entries+1]={key=key,path_hex=hex(path),bytes=size,sha256=hash}
    end
    return entries
end

function M.read()
    return M.parse(os.getenv('PSX_RUNTIME_DIRECTORY'),os.getenv('PSX_RUNTIME_MEDIA'))
end
function M.replacement(name)
    assert(type(name)=='string' and #name<=32 and name:match('^[a-z][a-z0-9-]*$'),'invalid replacement name')
    for _,entry in ipairs(M.read()) do
        if entry.key=='replacement:'..name..':track01.bin' then
            return os.getenv('PSX_RUNTIME_DIRECTORY')..'/replacements/'..name..'/disc/media.cue'
        end
    end
    error('undeclared replacement disc: '..name)
end
return M
-- )EOF"
