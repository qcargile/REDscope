local Bridge = {}

local function readFile(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

local function readJson(path)
    local s = readFile(path)
    if not s then return nil end
    local ok, t = pcall(json.decode, s)
    if not ok then return nil end
    return t
end

local moduleCache = {}

local function loadModule(requirePath, filePath)
    if moduleCache[requirePath] ~= nil then return moduleCache[requirePath] end
    local result = false
    local ok, mod = pcall(require, requirePath)
    if ok and type(mod) == "table" then
        result = mod
    elseif type(loadfile) == "function" then
        local chunk = loadfile(filePath)
        if chunk then
            local okRun, m2 = pcall(chunk)
            if okRun and type(m2) == "table" then result = m2 end
        end
    end
    moduleCache[requirePath] = result
    return result
end

function Bridge.loadAnalyzer()
    return loadModule("analyzer/core/analyzer", "analyzer/core/analyzer.lua")
end

function Bridge.loadCrashParser()
    return loadModule("analyzer/core/crashReportParser", "analyzer/core/crashReportParser.lua")
end

function Bridge.loadGrouping()
    return loadModule("analyzer/core/grouping", "analyzer/core/grouping.lua")
end

local ruleSetsCache = nil

function Bridge.liveRuleSets()
    if ruleSetsCache ~= nil then return ruleSetsCache end
    local engine = readJson("analyzer/rules/engine-signatures.json")
    local modr = readJson("analyzer/rules/mod-attribution.json")
    if not engine or not modr then return nil end
    ruleSetsCache = { engine, modr }
    return ruleSetsCache
end

function Bridge.conformanceData()
    local engine = readJson("analyzer/rules/engine-signatures.json")
    local modr = readJson("analyzer/rules/mod-attribution.json")
    local testr = readJson("analyzer/conformance/test-rules.json")
    local cases = readJson("analyzer/conformance/cases.json")
    if not (engine and modr and testr and cases) then return nil end
    return { sets = { engine, modr, testr }, cases = cases }
end

function Bridge.readLiveSidecar()
    local getter = Game["GetLatestCrashJson"]
    if type(getter) ~= "function" then return nil, "native_unbound" end
    local ok, content = pcall(getter)
    if not ok or type(content) ~= "string" then return nil, "native_bad_return" end
    if content == "" then return nil, "no_crash" end
    local okDec, sidecar = pcall(json.decode, content)
    if not okDec or type(sidecar) ~= "table" then return nil, "decode_failed" end
    return sidecar, nil, #content
end

function Bridge.analyzeLatest()
    local Analyzer = Bridge.loadAnalyzer()
    if type(Analyzer) ~= "table" then return nil, nil, "analyzer_load_failed" end
    local sets = Bridge.liveRuleSets()
    if not sets then return nil, nil, "rules_load_failed" end

    local text, textReason = Bridge.readLatestCrashText()
    local sidecar, jsonReason, bytes = Bridge.readLiveSidecar()

    if text ~= nil then
        local Parser = Bridge.loadCrashParser()
        if type(Parser) == "table" then
            local parsed = Parser.parseFromCombined(text)
            if type(parsed) == "table" then
                if sidecar ~= nil and sidecar.crashId == parsed.crashId then
                    return Analyzer.analyze(sidecar, sets), sidecar, nil, bytes
                end
                return Analyzer.analyze(parsed, sets), parsed, nil
            end
        end
    end

    if sidecar ~= nil then
        return Analyzer.analyze(sidecar, sets), sidecar, nil, bytes
    end

    return nil, nil, textReason or jsonReason
end

function Bridge.readLatestCrashText()
    local getter = Game["GetLatestCrashReport"]
    if type(getter) ~= "function" then return nil, "native_unbound" end
    local ok, content = pcall(getter)
    if not ok or type(content) ~= "string" then return nil, "native_bad_return" end
    if content == "" then return nil, "no_crash" end
    return content, nil
end


function Bridge.readAllSidecars()
    local getter = Game["GetAllCrashesJson"]
    if type(getter) ~= "function" then return nil, "native_unbound" end
    local ok, content = pcall(getter)
    if not ok or type(content) ~= "string" then return nil, "native_bad_return" end
    local okDec, list = pcall(json.decode, content)
    if not okDec or type(list) ~= "table" then return nil, "decode_failed" end
    return list, nil
end

function Bridge.analyzeAll()
    local Grouping = Bridge.loadGrouping()
    if type(Grouping) ~= "table" then return nil, "grouping_load_failed" end
    local summaries, reason = Bridge.readAllSidecars()
    if not summaries then return nil, reason end
    return Grouping.analyze(summaries), nil
end

function Bridge.groupingData()
    return readJson("analyzer/conformance/grouping-cases.json")
end

return Bridge
