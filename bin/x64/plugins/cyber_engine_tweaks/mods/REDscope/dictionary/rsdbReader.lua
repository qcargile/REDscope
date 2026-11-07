local RsdbReader = {}

local function readAt(f, offset, n)
    f:seek("set", offset)
    return f:read(n)
end

local function u32le(s, i)
    local b1, b2, b3, b4 = s:byte(i, i + 3)
    return b1 + b2 * 256 + b3 * 65536 + b4 * 16777216
end

function RsdbReader.open(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local header = readAt(f, 0, 16)
    if not header or #header < 16 or header:sub(1, 4) ~= "RSD1" then
        f:close()
        return nil
    end
    return {
        f = f,
        count = u32le(header, 5),
        indexOffset = u32le(header, 9),
        pathsOffset = u32le(header, 13),
    }
end

local function hexToBE(hexHash)
    local h = hexHash:gsub("^0[xX]", "")
    if #h < 16 then h = ("0"):rep(16 - #h) .. h end
    local bytes = {}
    for i = 1, 16, 2 do
        bytes[#bytes + 1] = string.char(tonumber(h:sub(i, i + 1), 16))
    end
    return table.concat(bytes)
end

function RsdbReader.lookupBytes(dict, target)
    local lo, hi = 0, dict.count - 1
    while lo <= hi do
        local mid = math.floor((lo + hi) / 2)
        local entry = readAt(dict.f, dict.indexOffset + mid * 16, 16)
        if not entry or #entry < 16 then return nil end
        local key = entry:sub(1, 8)
        if key == target then
            local pathOff = u32le(entry, 9)
            local pathLen = u32le(entry, 13)
            return readAt(dict.f, dict.pathsOffset + pathOff, pathLen)
        elseif key < target then
            lo = mid + 1
        else
            hi = mid - 1
        end
    end
    return nil
end

function RsdbReader.lookup(dict, hexHash)
    return RsdbReader.lookupBytes(dict, hexToBE(hexHash))
end

function RsdbReader.close(dict)
    if dict and dict.f then dict.f:close() end
end

return RsdbReader
