local Bridge = require("analyzerBridge")
local Panel = require("panel")
local Diagnostic = require("diagnostic")
local GameUI = require("GameUI")

local REDscope = {}

local cachedSetState = nil
local setStateLookupAttempted = false

local function ResolveSetState()
    if setStateLookupAttempted then return cachedSetState end
    setStateLookupAttempted = true
    for _, key in ipairs({"SetState", "SetState;StringString", "REDscope.SetState"}) do
        local ok, fn = pcall(function() return Game[key] end)
        if ok and fn then cachedSetState = fn; return cachedSetState end
    end
    return nil
end

function REDscope.SetState(key, value)
    if type(key) ~= "string" then key = tostring(key) end
    if type(value) ~= "string" then value = tostring(value) end
    local fn = ResolveSetState()
    if not fn then return end
    pcall(fn, key, value)
end

local lastPollSec = 0
local lastState = {}
local function pushIfChanged(key, value)
    if lastState[key] == value then return end
    lastState[key] = value
    REDscope.SetState(key, value)
end

local settingsAccum = 15
local function pushSettings()
    local ok, settings = pcall(Diagnostic.readSettings)
    if not ok or type(settings) ~= "table" then return end
    for _, s in ipairs(settings) do
        pushIfChanged(s.label, s.value)
    end
end

local function safeCall(target, method)
    if target == nil then return nil end
    local ok, v = pcall(function() return target[method](target) end)
    if ok then return v end
    return nil
end

local function getDistrict()
    local prevention = Game.GetScriptableSystemsContainer():Get("PreventionSystem")
    if prevention == nil then return nil end
    local dm = prevention.districtManager
    if dm == nil or dm:GetCurrentDistrict() == nil then return nil end
    local id = dm:GetCurrentDistrict():GetDistrictID()
    local tdb = GetSingleton("gamedataTweakDBInterface")
    local rec = tdb:GetDistrictRecord(id)
    if rec == nil then return nil end
    local labels = {}
    repeat
        table.insert(labels, 1, Game.GetLocalizedText(rec:LocalizedName()))
        rec = rec:ParentDistrict()
    until rec == nil
    return table.concat(labels, " / ")
end

local function pushContext(state)
    pushIfChanged("loading", state.isLoading and "yes" or "no")
    pushIfChanged("sessionActive", state.isLoaded and "yes" or "no")
    pushIfChanged("scene", state.isScene and "yes" or "no")
    pushIfChanged("johnny", (state.isJohnny or state.isPossessed) and "yes" or "no")
    pushIfChanged("menu", (type(state.menu) == "string") and state.menu or "none")
    pushIfChanged("vehicle", state.isVehicle and "yes" or "no")
    pushIfChanged("fastTravel", state.isFastTravel and "yes" or "no")
end

local function PollGameState(deltaTime)
    lastPollSec = lastPollSec + (deltaTime or 0)
    if lastPollSec < 2.0 then return end
    lastPollSec = 0

    local player = Game.GetPlayer()
    if player == nil then
        pushIfChanged("phase", "no-player")
        return
    end
    pushIfChanged("phase", "in-game")

    local inCombat = safeCall(player, "IsInCombat")
    if inCombat ~= nil then pushIfChanged("combat", inCombat and "yes" or "no") end

    local mounted = safeCall(player, "IsMounted")
    if mounted ~= nil then pushIfChanged("mounted", mounted and "yes" or "no") end

    local pos = safeCall(player, "GetWorldPosition")
    if pos ~= nil then
        local ok, str = pcall(function()
            return string.format("%.0f,%.0f,%.0f", pos.x or 0, pos.y or 0, pos.z or 0)
        end)
        if ok and str then pushIfChanged("worldPos", str) end
    end

    local okD, district = pcall(getDistrict)
    if okD and district ~= nil and district ~= "" then pushIfChanged("district", district) end

    local questsSys = Game.GetQuestsSystem and Game.GetQuestsSystem() or nil
    if questsSys ~= nil then
        local tracked = safeCall(questsSys, "GetTrackedQuest")
        if tracked ~= nil then
            local id = safeCall(tracked, "GetId")
            if id ~= nil then pushIfChanged("trackedQuest", tostring(id)) end
        end
    end

    settingsAccum = settingsAccum + 1
    if settingsAccum >= 15 then
        settingsAccum = 0
        pushSettings()
    end
end

function REDscope.Diagnose()
    print("[REDscope] === Diagnose start ===")
    local Analyzer = Bridge.loadAnalyzer()
    if type(Analyzer) ~= "table" then
        print("[REDscope] FAIL loading analyzer.lua (require 'analyzer/core/analyzer' + loadfile both failed).")
        return
    end
    local conf = Bridge.conformanceData()
    if not conf then
        print("[REDscope] FAIL loading analyzer data files via io (mod-dir relative).")
        return
    end
    local function eqArr(a, b)
        if #a ~= #b then return false end
        for i = 1, #a do if a[i] ~= b[i] then return false end end
        return true
    end
    local pass, fail = 0, 0
    for _, c in ipairs(conf.cases) do
        local r = Analyzer.analyze(c.sidecar, conf.sets)
        local ids = {}
        for _, m in ipairs(r.matches) do ids[#ids + 1] = m.id end
        local top = r.topMatch and r.topMatch.id or nil
        local leads = {}
        for _, l in ipairs(r.leads) do leads[#leads + 1] = l.module end
        local e = c.expected
        if eqArr(ids, e.matchIds) and top == e.topMatchId and eqArr(leads, e.leadModules or {}) then
            pass = pass + 1
        else
            fail = fail + 1
            print("[REDscope] CASE FAIL " .. tostring(c.name)
                .. " | top=" .. tostring(top) .. " want=" .. tostring(e.topMatchId)
                .. " | ids=" .. table.concat(ids, ",")
                .. " | leads=" .. table.concat(leads, ","))
        end
    end
    print("[REDscope] Lua analyzer conformance: " .. pass .. " passed, " .. fail .. " failed of " .. #conf.cases)
    if fail == 0 then
        print("[REDscope] analyzer.lua matches the JS reference under CET (twin-matcher verified).")
    end

    local Grouping = Bridge.loadGrouping()
    local gcases = Bridge.groupingData()
    if type(Grouping) == "table" and gcases ~= nil then
        local gpass, gfail = 0, 0
        for _, c in ipairs(gcases) do
            local r = Grouping.analyze(c.summaries)
            local e = c.expected
            local gok = (r.distinctCount == e.distinctCount) and (r.totalCount == e.totalCount)
            for i, id in ipairs(e.order or {}) do
                if not r.groups[i] or r.groups[i].crashId ~= id then gok = false end
            end
            for id, n in pairs(e.counts or {}) do
                if not r.byCrashId[id] or r.byCrashId[id].count ~= n then gok = false end
            end
            for m, n in pairs(e.cooc or {}) do
                if r.cooccurrence[m] ~= n then gok = false end
            end
            for lid, n in pairs(e.looseCounts or {}) do
                if r.looseCounts[lid] ~= n then gok = false end
            end
            if gok then
                gpass = gpass + 1
            else
                gfail = gfail + 1
                print("[REDscope] GROUPING CASE FAIL " .. tostring(c.name))
            end
        end
        print("[REDscope] Lua grouping conformance: " .. gpass .. " passed, " .. gfail .. " failed of " .. #gcases)
    else
        print("[REDscope] grouping module or cases not loaded - skipping grouping conformance.")
    end

    local result, sidecar, reason, bytes = Bridge.analyzeLatest()
    if not result then
        if reason == "native_unbound" then
            print("[REDscope] native GetLatestCrashJson not bound (old DLL, or registration failed) - skipping live read.")
        elseif reason == "native_bad_return" then
            print("[REDscope] native GetLatestCrashJson did NOT return a string (marshalling).")
        elseif reason == "no_crash" then
            print("[REDscope] native bound OK and String marshalling WORKS, but no .crash.json on disk yet. Trigger a crash to validate the full read path.")
        elseif reason == "decode_failed" then
            print("[REDscope] FAIL json.decode of live crash content.")
        else
            print("[REDscope] live read unavailable: " .. tostring(reason))
        end
        print("[REDscope] === Diagnose done ===")
        return
    end
    local smCount = 0
    if type(sidecar.stackModules) == "table" then smCount = #sidecar.stackModules end
    print("[REDscope] LIVE crash: id=" .. tostring(result.crashId)
        .. " bytes=" .. tostring(bytes)
        .. " schema=" .. tostring(sidecar.schema)
        .. " exc=" .. tostring(result.facts.exceptionCode)
        .. " faultMod=" .. tostring(result.facts.faultingModule)
        .. " stackModules=" .. smCount)
    local topId = result.topMatch and result.topMatch.id or "(no rule matched - facts-only floor)"
    print("[REDscope] LIVE topMatch: " .. tostring(topId))
    for _, m in ipairs(result.matches) do
        print("[REDscope]   rule " .. tostring(m.id) .. " [" .. tostring(m.verdict) .. "/" .. tostring(m.confidence) .. "]")
    end
    if #result.leads > 0 then
        for _, l in ipairs(result.leads) do
            print("[REDscope]   lead: " .. tostring(l.module) .. " (" .. tostring(l.hits) .. " hits) - " .. tostring(l.why))
        end
    else
        print("[REDscope]   no mod leads in the crash stack.")
    end
    local grouping = Bridge.analyzeAll()
    if grouping then
        print("[REDscope] LIVE grouping: " .. tostring(grouping.distinctCount) .. " distinct / " .. tostring(grouping.totalCount) .. " total crashes")
        local cur = grouping.byCrashId[result.crashId]
        if cur then
            print("[REDscope]   this crash seen " .. tostring(cur.count) .. "x (first " .. tostring(cur.firstSeen) .. " last " .. tostring(cur.lastSeen) .. ")")
        end
    end
    print("[REDscope] === Diagnose done ===")
end

registerForEvent("onInit", function()
    local ok = pcall(function() GameUI.Listen(pushContext) end)
    if not ok then
        print("[REDscope] GameUI.Listen failed; scene/loading/johnny context state disabled.")
    end
end)
registerForEvent("onOverlayOpen", Panel.onOverlayOpen)
registerForEvent("onOverlayClose", Panel.onOverlayClose)
registerForEvent("onDraw", Panel.draw)
registerForEvent("onUpdate", PollGameState)

return REDscope
