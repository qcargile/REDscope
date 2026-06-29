local Bridge = require("analyzerBridge")
local Diagnostic = require("diagnostic")

local Panel = {}

local COL_DIM = { 0.62, 0.60, 0.57, 1.0 }
local COL_HEAD = { 0.55, 0.82, 0.95, 1.0 }
local COL_HIGH = { 0.93, 0.42, 0.38, 1.0 }
local COL_MED = { 0.95, 0.78, 0.35, 1.0 }
local COL_LOW = { 0.70, 0.74, 0.78, 1.0 }

local function pushCol(c)
    ImGui.PushStyleColor(ImGuiCol.Text, c[1], c[2], c[3], c[4])
end

local function textCol(c, s)
    pushCol(c)
    ImGui.Text(s)
    ImGui.PopStyleColor()
end

local function wrappedCol(c, s)
    pushCol(c)
    ImGui.TextWrapped(s)
    ImGui.PopStyleColor()
end

local function fmtTime(unixSecs)
    local t = tonumber(unixSecs)
    if t == nil or t <= 0 then return "unknown" end
    if type(os) ~= "table" or type(os.date) ~= "function" then return tostring(t) end
    local ok, s = pcall(os.date, "%Y-%m-%d %H:%M", t)
    if ok and type(s) == "string" then return s end
    return tostring(t)
end

local function sevColor(sev)
    sev = sev or 0
    if sev >= 5 then return COL_HIGH end
    if sev >= 3 then return COL_MED end
    return COL_LOW
end

local function confColor(conf)
    if conf == "high" then return COL_HIGH end
    if conf == "medium" then return COL_MED end
    return COL_LOW
end

local function linkText(link)
    if type(link) == "table" then
        if link.url ~= nil then
            if link.label ~= nil then return tostring(link.label) .. ": " .. tostring(link.url) end
            return tostring(link.url)
        end
        return nil
    end
    return tostring(link)
end

local function renderLinks(links)
    if type(links) ~= "table" then return end
    for _, l in ipairs(links) do
        local t = linkText(l)
        if t ~= nil and t ~= "" then
            wrappedCol(COL_DIM, t)
        end
    end
end

local function renderFacts(result, sidecar)
    if sidecar.source == "crash-text-fallback" then
        wrappedCol(COL_MED, "Degraded mode: this crash has no .crash.json sidecar; parsing the .crash text. Some fields (installed mods, full stack memory) are unavailable.")
        ImGui.Separator()
    end
    textCol(COL_HEAD, "Crash " .. tostring(result.crashId or "?"))
    local exc = result.facts.exceptionCode or "?"
    local class = result.facts.crashClass or "Unknown"
    ImGui.Text(tostring(exc) .. "    " .. tostring(class))
    local fm = result.facts.faultingModule
    if fm ~= nil then
        ImGui.Text("Faulting module: " .. tostring(fm))
    else
        textCol(COL_DIM, "Faulting module: (none resolved)")
    end
    if type(sidecar.fingerprint) == "table" and sidecar.fingerprint.faultSiteUnreliable == true then
        wrappedCol(COL_DIM, "The faulting module is the symptom site, not necessarily the cause.")
    end
    if sidecar.likelyCulprit ~= nil and sidecar.likelyCulprit ~= "" then
        wrappedCol(COL_DIM, "REDscope first-pass lead: " .. tostring(sidecar.likelyCulprit))
    end
    if type(sidecar.buildId) == "string" and sidecar.buildId ~= "" then
        textCol(COL_DIM, "Game build: " .. sidecar.buildId)
    end
end

local function renderLeadsList(result, grouping)
    if grouping == nil or grouping.distinctCount == nil or grouping.distinctCount == 0 then
        for _, l in ipairs(result.leads or {}) do
            ImGui.Text(tostring(l.module))
            ImGui.SameLine()
            textCol(COL_DIM, "seen " .. tostring(l.hits or 0) .. "x")
            wrappedCol(COL_DIM, tostring(l.why or "present in the crash stack") .. " - not a confirmed cause.")
        end
        return
    end
    local smallSample = grouping.distinctCount < 5
    for _, l in ipairs(result.leads) do
        ImGui.Text(tostring(l.module))
        ImGui.SameLine()
        textCol(COL_DIM, "seen " .. tostring(l.hits or 0) .. "x")
        if grouping ~= nil and grouping.distinctCount ~= nil and grouping.distinctCount > 1 then
            local m = grouping.cooccurrence[string.lower(tostring(l.module))]
            if m ~= nil and m >= 2 then
                local label = "present in " .. tostring(m) .. " of your " .. tostring(grouping.distinctCount) .. " distinct crashes - investigation lead, not confirmed cause"
                if smallSample then label = label .. " (small sample - interpret with caution)" end
                textCol(COL_MED, label)
            end
        end
        wrappedCol(COL_DIM, tostring(l.why or "present in the crash stack") .. " - not a confirmed cause.")
    end
end

local function renderRecurrencePill(result, grouping)
    if grouping == nil or result.crashId == nil then return end
    local g = grouping.byCrashId[result.crashId]
    if g == nil or g.count <= 1 then return end
    textCol(COL_HIGH, "Seen " .. tostring(g.count) .. " times (last " .. fmtTime(g.lastSeen) .. ")")
end

local function renderRecurrence(result, grouping)
    if grouping == nil or result.crashId == nil then return end
    local g = grouping.byCrashId[result.crashId]
    if g == nil then return end
    ImGui.Separator()
    if g.count <= 1 then
        textCol(COL_DIM, "First time seeing this crash.")
    else
        textCol(COL_DIM, "first seen " .. fmtTime(g.firstSeen) .. "   last seen " .. fmtTime(g.lastSeen))
    end
    if g.looseId ~= nil and g.looseId ~= "" then
        local distinctAtSite = grouping.looseCounts[g.looseId]
        if distinctAtSite ~= nil and distinctAtSite > 1 then
            textCol(COL_DIM, "this fault-site fingerprint matches " .. tostring(distinctAtSite) .. " distinct crashes in your history")
        end
    end
end

local function renderGameState(sidecar)
    if type(sidecar.gameStateLive) ~= "table" then return end
    local kvs = {}
    for k, v in pairs(sidecar.gameStateLive) do
        if type(k) == "string" and type(v) == "string" and v ~= "" then
            kvs[#kvs + 1] = { k = k, v = v }
        end
    end
    if #kvs == 0 then return end
    table.sort(kvs, function(a, b) return a.k < b.k end)
    ImGui.Separator()
    textCol(COL_HEAD, "Live game state (pushed via REDscope.SetState)")
    for _, kv in ipairs(kvs) do
        ImGui.Text(kv.k)
        ImGui.SameLine(160)
        textCol(COL_DIM, kv.v)
    end
end

local function renderModsChanged(sidecar)
    local d = sidecar.modsChangedSinceLastLaunch
    if type(d) ~= "table" then return end
    local added = type(d.added) == "table" and d.added or {}
    local updated = type(d.updated) == "table" and d.updated or {}
    local removed = type(d.removed) == "table" and d.removed or {}
    if #added == 0 and #updated == 0 and #removed == 0 then return end
    ImGui.Separator()
    textCol(COL_MED, "Mods changed since your last launch")
    wrappedCol(COL_DIM, "A mod that changed right before a new crash is the strongest local lead - not proof.")
    for _, m in ipairs(added) do
        local v = (type(m.version) == "string" and m.version ~= "") and ("  " .. m.version) or ""
        textCol(COL_HIGH, "+ added: " .. tostring(m.name or "?") .. v)
    end
    for _, m in ipairs(updated) do
        textCol(COL_MED, "~ updated: " .. tostring(m.name or "?") .. "  " .. tostring(m.from or "?") .. " -> " .. tostring(m.to or "?"))
    end
    for _, m in ipairs(removed) do
        textCol(COL_DIM, "- removed: " .. tostring(m.name or "?"))
    end
end

local function renderMatches(result)
    ImGui.Separator()
    if #result.matches == 0 then
        wrappedCol(COL_DIM, "No diagnostic rule matched this crash. The raw facts above are all REDscope can state.")
        return
    end
    for _, m in ipairs(result.matches) do
        pushCol(sevColor(m.severity))
        ImGui.Text("[" .. tostring(m.verdict or "?") .. "]")
        ImGui.PopStyleColor()
        ImGui.SameLine()
        pushCol(confColor(m.confidence))
        ImGui.Text(string.upper(tostring(m.confidence or "?")) .. " confidence")
        ImGui.PopStyleColor()
        if m.name ~= nil and m.name ~= "" then
            ImGui.TextWrapped(tostring(m.name))
        end
        if m.message ~= nil and m.message ~= "" then
            ImGui.TextWrapped(tostring(m.message))
        end
        if m.fix ~= nil and m.fix ~= "" then
            wrappedCol(COL_DIM, "Fix: " .. tostring(m.fix))
        end
        renderLinks(m.links)
        ImGui.Separator()
    end
end

local function renderLeads(result, grouping)
    if ImGui.CollapsingHeader("Leads - mod modules in this crash's stack") then
        if #result.leads == 0 then
            wrappedCol(COL_DIM, "No mod modules were present in the crash stack.")
        else
            renderLeadsList(result, grouping)
        end
    end
end

local function renderStackDetail(sidecar)
    if ImGui.CollapsingHeader("All modules in the crash stack") then
        local sm = sidecar.stackModules
        if type(sm) ~= "table" or #sm == 0 then
            wrappedCol(COL_DIM, "No modules resolved from the crash stack.")
        else
            for _, m in ipairs(sm) do
                ImGui.Text(tostring(m.name) .. "  (" .. tostring(m.kind) .. ", " .. tostring(m.hits) .. " hits)")
            end
        end
        local sf = sidecar.scriptedFrames
        if type(sf) == "table" and #sf > 0 then
            ImGui.Separator()
            textCol(COL_DIM, "Scripted frames:")
            for _, f in ipairs(sf) do
                ImGui.Text(tostring(f))
            end
        end
    end
end

local function renderObjectsInFlight(sidecar)
    local oif = sidecar.objectsInFlight
    if type(oif) ~= "table" or #oif == 0 then return end
    if ImGui.CollapsingHeader("Objects in the crashing thread's registers") then
        wrappedCol(COL_DIM, "RTTI classes REDscope identified in the CPU registers at the fault - what the engine was operating on.")
        for _, o in ipairs(oif) do
            ImGui.Text(tostring(o.reg or "?") .. "  " .. tostring(o.className or "?"))
        end
    end
end

local function renderEngineState(sidecar)
    local e = sidecar.engineState
    local hw = sidecar.hardware
    if type(e) ~= "table" and type(hw) ~= "table" then return end
    if ImGui.CollapsingHeader("Engine + hardware state at crash") then
        if type(e) == "table" then
            if e.state ~= nil then
                local s = "Engine: " .. tostring(e.state)
                if e.scriptsLoaded ~= nil then s = s .. "   scripts " .. (e.scriptsLoaded and "loaded" or "not loaded") end
                if e.isEP1 then s = s .. "   EP1" end
                ImGui.Text(s)
            end
            if e.gpuUsedMB ~= nil and e.gpuBudgetMB ~= nil then
                local used = tonumber(e.gpuUsedMB) or 0
                local budget = tonumber(e.gpuBudgetMB) or 0
                local pct = (budget > 0) and math.floor(used * 100 / budget) or 0
                local line = string.format("VRAM at crash: %d / %d MB  (%d%% of budget)", used, budget, pct)
                if pct >= 90 then textCol(COL_HIGH, line) else ImGui.Text(line) end
            end
            if e.oomSuspected == true then
                textCol(COL_HIGH, "Out of memory suspected: " .. tostring(e.oomBasis or "memory near limit"))
            end
        end
        if type(hw) == "table" then
            local gpu = hw.gpu and tostring(hw.gpu) or "(unknown)"
            local drv = hw.gpuDriver and ("   driver " .. tostring(hw.gpuDriver)) or ""
            local vram = hw.vramMB and ("   " .. tostring(hw.vramMB) .. " MB") or ""
            textCol(COL_DIM, "GPU: " .. gpu .. drv .. vram)
            if hw.cpu ~= nil then
                local c = "CPU: " .. tostring(hw.cpu)
                if hw.ramTotalMB ~= nil then c = c .. "   -   " .. tostring(hw.ramTotalMB) .. " MB RAM" end
                textCol(COL_DIM, c)
            end
        end
    end
end

local function renderCallStackMods(sidecar)
    local cm = sidecar.callStackMods
    if type(cm) ~= "table" or #cm == 0 then return end
    if ImGui.CollapsingHeader("Mod DLLs on the crashing thread's call stack") then
        wrappedCol(COL_DIM, "Unwound from the call stack - stronger than the stack-memory scan above. Framework DLLs (CET, RED4ext, Codeware) ride most crash stacks; that's normal, not a lead.")
        for _, m in ipairs(cm) do
            ImGui.Text(tostring(m.name) .. "  (frame " .. tostring(m.topFrame or "?") .. ")")
        end
    end
end

local function renderLedgerArray(sidecar, key, title, fields)
    local arr = sidecar[key]
    if type(arr) ~= "table" or #arr == 0 then return end
    if ImGui.CollapsingHeader(title .. " (" .. #arr .. ")") then
        for _, o in ipairs(arr) do
            local parts = {}
            for _, f in ipairs(fields) do
                if o[f] ~= nil then parts[#parts + 1] = tostring(o[f]) end
            end
            ImGui.Text("  " .. table.concat(parts, "   "))
        end
    end
end

local function renderConflictsPanel(sidecar)
    local si = sidecar.setupIntegrity
    local ac = sidecar.archiveConflicts
    local hasSi = type(si) == "table" and type(si.issues) == "table" and #si.issues > 0
    local curated = (type(ac) == "table") and ac.curated or nil
    local hasAc = type(curated) == "table" and
        ((type(curated.active) == "table" and #curated.active > 0) or
         (type(curated.missingDeps) == "table" and #curated.missingDeps > 0))
    if not hasSi and not hasAc then return end
    if ImGui.CollapsingHeader("Install problems + conflicts") then
        if hasSi then
            for _, i in ipairs(si.issues) do
                textCol(COL_MED, "  [" .. tostring(i.kind) .. "] " .. tostring(i.detail))
            end
        end
        if hasAc then
            for _, g in ipairs(curated.active or {}) do
                local mods = {}
                for _, m in ipairs(g.mods or {}) do mods[#mods + 1] = tostring(m) end
                textCol(COL_MED, "  Conflict: " .. table.concat(mods, ", "))
            end
            for _, d in ipairs(curated.missingDeps or {}) do
                local req = {}
                for _, r in ipairs(d.requires or {}) do req[#req + 1] = tostring(r) end
                textCol(COL_MED, "  " .. tostring(d.mod) .. " requires " .. table.concat(req, ", "))
            end
        end
    end
end

local function renderWrapChains(sidecar)
    local wc = sidecar.wrapChains
    if type(wc) ~= "table" or #wc == 0 then return end
    if ImGui.CollapsingHeader("Mod method-wraps on the crashing stack") then
        wrappedCol(COL_DIM, "@wrapMethod layers stacked on methods in the crash stack - which mods wrap each method, and where (file:line). The most actionable conflict lead; shown for any crash, not only when a rule matched.")
        for _, c in ipairs(wc) do
            textCol(COL_HEAD, tostring(c.methodKey or "?"))
            if type(c.layers) == "table" then
                for _, l in ipairs(c.layers) do
                    ImGui.Text("    " .. tostring(l.modName or "?") .. "   " .. tostring(l.file or "?") .. ":" .. tostring(l.line or "?"))
                end
            end
        end
    end
end

local function renderHistory(result, grouping)
    if grouping == nil or grouping.distinctCount == nil or grouping.distinctCount == 0 then return end
    local hdr = "Crash history - " .. tostring(grouping.distinctCount) .. " distinct, " .. tostring(grouping.totalCount) .. " total"
    if ImGui.CollapsingHeader(hdr) then
        for _, g in ipairs(grouping.groups) do
            local isCurrent = (result.crashId ~= nil and g.crashId == result.crashId)
            local label = tostring(g.label or g.crashId) .. "  x" .. tostring(g.count)
            if isCurrent then
                textCol(COL_HEAD, "> " .. label .. "  (this crash)")
            else
                ImGui.Text("  " .. label)
            end
            textCol(COL_DIM, "    last seen " .. fmtTime(g.lastSeen) .. "  -  " .. tostring(g.crashId))
        end
    end
end

local function buildSummary(result, sidecar)
    local lines = {}
    lines[#lines + 1] = "REDscope crash analysis"
    lines[#lines + 1] = "Crash ID: " .. tostring(result.crashId or "?")
    if type(sidecar.buildId) == "string" and sidecar.buildId ~= "" then
        lines[#lines + 1] = "Game build: " .. sidecar.buildId
    end
    local exc = result.facts.exceptionCode or "?"
    local class = result.facts.crashClass or "Unknown"
    lines[#lines + 1] = "Exception: " .. tostring(exc) .. " (" .. tostring(class) .. ")"
    local fm = result.facts.faultingModule
    lines[#lines + 1] = "Faulting module: " .. (fm ~= nil and tostring(fm) or "(none resolved)")
    if #result.matches == 0 then
        lines[#lines + 1] = "Matched rule: none (facts only)"
    else
        for _, m in ipairs(result.matches) do
            lines[#lines + 1] = string.format("Rule [%s/%s]: %s",
                tostring(m.verdict), tostring(m.confidence), tostring(m.message or m.name or m.id))
        end
    end
    if #result.leads > 0 then
        local mods = {}
        for _, l in ipairs(result.leads) do
            mods[#mods + 1] = tostring(l.module) .. " (" .. tostring(l.hits or 0) .. "x)"
        end
        lines[#lines + 1] = "Leads (in stack, not confirmed): " .. table.concat(mods, ", ")
    end
    local d = sidecar.modsChangedSinceLastLaunch
    if type(d) == "table" then
        local parts = {}
        if type(d.added) == "table" then for _, m in ipairs(d.added) do parts[#parts + 1] = "+" .. tostring(m.name) end end
        if type(d.updated) == "table" then for _, m in ipairs(d.updated) do parts[#parts + 1] = "~" .. tostring(m.name) end end
        if type(d.removed) == "table" then for _, m in ipairs(d.removed) do parts[#parts + 1] = "-" .. tostring(m.name) end end
        if #parts > 0 then lines[#lines + 1] = "Mods changed since last launch: " .. table.concat(parts, ", ") end
    end
    lines[#lines + 1] = "Leads, not a verdict. Generated by REDscope."
    return table.concat(lines, "\n")
end

local diagState = { report = nil, status = nil }

local function renderDiagnostic()
    ImGui.Separator()
    if ImGui.CollapsingHeader("Diagnostic report (system + settings + modlist)") then
        wrappedCol(COL_DIM, "An on-demand report of your specs, in-game settings, and modlist - for sharing when you ask for mod help. No crash needed.")
        if ImGui.Button("Generate diagnostic report") then
            local report, reason = Diagnostic.build()
            if report ~= nil then
                diagState.report = report
                diagState.status = string.format("Generated: %d enabled / %d mods, %d settings captured.",
                    report.enabledCount, report.total, report.settingsCount)
            else
                diagState.report = nil
                diagState.status = "Could not generate (" .. tostring(reason) .. ")."
            end
        end
        if diagState.status ~= nil then textCol(COL_DIM, diagState.status) end
        if diagState.report ~= nil then
            if ImGui.Button("Copy report") then
                if Diagnostic.copy(diagState.report) then
                    diagState.status = "Copied the report to your clipboard."
                end
            end
            ImGui.SameLine()
            if ImGui.Button("Download report") then
                local name = Diagnostic.download(diagState.report)
                if name ~= nil then
                    diagState.status = "Saved " .. name .. " in the REDscope mod folder."
                else
                    diagState.status = "Could not write the report file."
                end
            end
        end
    end
end

local symState = { auto = nil, status = nil }

local function symCall(keys, ...)
    for _, k in ipairs(keys) do
        local ok, fn = pcall(function() return Game[k] end)
        if ok and fn then
            local rok, r = pcall(fn, ...)
            return true, (rok and r or nil)
        end
    end
    return false, nil
end

local function renderSymbols()
    ImGui.Separator()
    if ImGui.CollapsingHeader("Debug symbols (named crash frames)") then
        wrappedCol(COL_DIM, "Some framework mods publish debug symbols (.pdb) on GitHub. With them, native crash frames show real function names like RED4ext.dll!PluginSystem::Load instead of module+offset. Covered: RED4ext, CET, Mod Settings, Audioware, Input Loader. The game's own frames stay as offsets - CDPR ships no symbols for the executable.")
        if symState.auto == nil then
            local _, v = symCall({ "GetAutoDownload", "REDscope.GetAutoDownload" })
            symState.auto = v and true or false
        end
        local newAuto, changed = ImGui.Checkbox("Auto-download symbols on launch", symState.auto)
        symState.auto = newAuto and true or false
        if changed then
            symCall({ "SetAutoDownload", "SetAutoDownload;Bool", "REDscope.SetAutoDownload" }, symState.auto)
            symState.status = symState.auto
                and "On. Takes effect next launch - REDscope fetches matching symbols at startup."
                or "Off."
        end
        wrappedCol(COL_HIGH, "WARNING: turning this on makes REDscope download .pdb files from the framework mods' official GitHub release pages at launch. It is OFF by default. If you'd rather not, leave it off and drop the .pdb files into the symbols folder yourself.")
        if ImGui.Button("Download now") then
            local reached = symCall({ "DownloadSymbolsNow", "REDscope.DownloadSymbolsNow" })
            symState.status = reached
                and "Download started in the background. Check your next crash report or the REDscope log for results."
                or "Couldn't reach the download function - is the current REDscope.dll loaded?"
        end
        if symState.status ~= nil then textCol(COL_DIM, symState.status) end
    end
end

local state = {
    open = false,
    analyzed = false,
    result = nil,
    sidecar = nil,
    err = nil,
    grouping = nil,
}

local function refresh()
    local ok, r, s, e = pcall(Bridge.analyzeLatest)
    if ok then
        state.result, state.sidecar, state.err = r, s, e
    else
        state.result, state.sidecar, state.err = nil, nil, "analyzer_threw"
    end
    local okG, g = pcall(Bridge.analyzeAll)
    if okG and type(g) == "table" then state.grouping = g else state.grouping = nil end
    state.analyzed = true
end

function Panel.onOverlayOpen()
    state.open = true
    if not state.analyzed then refresh() end
end

function Panel.onOverlayClose()
    state.open = false
end

local ERR_MSG = {
    native_unbound = "REDscope's crash-data function isn't available yet. Make sure the current REDscope.dll is loaded.",
    native_bad_return = "REDscope returned crash data in an unexpected form.",
    no_crash = "No crash has been recorded yet. Nothing to analyze - that's a good sign.",
    decode_failed = "A crash file was found but couldn't be read.",
    analyzer_load_failed = "The analyzer engine (analyzer/core/analyzer.lua) didn't load.",
    rules_load_failed = "The analyzer rule files didn't load.",
    analyzer_threw = "The analyzer hit an unexpected error while reading the crash.",
}

local function renderError(err)
    if err == nil then
        wrappedCol(COL_DIM, "No crash analysis is available yet.")
        return
    end
    wrappedCol(COL_DIM, ERR_MSG[err] or ("Unavailable: " .. tostring(err)))
end

function Panel.draw()
    if not state.open then return end
    ImGui.SetNextWindowPos(60, 60, ImGuiCond.FirstUseEver)
    ImGui.SetNextWindowSize(540, 480, ImGuiCond.FirstUseEver)
    if ImGui.Begin("REDscope - Last Crash") then
        if state.result ~= nil and state.sidecar ~= nil then
            renderFacts(state.result, state.sidecar)
            renderRecurrencePill(state.result, state.grouping)
            renderRecurrence(state.result, state.grouping)
            renderGameState(state.sidecar)
            renderEngineState(state.sidecar)
            renderModsChanged(state.sidecar)
            renderMatches(state.result)
            renderLeads(state.result, state.grouping)
            renderObjectsInFlight(state.sidecar)
            renderStackDetail(state.sidecar)
            renderCallStackMods(state.sidecar)
            renderWrapChains(state.sidecar)
            renderLedgerArray(state.sidecar, "recentDelays", "Active delays", { "callbackClass", "tag", "delayId" })
            renderLedgerArray(state.sidecar, "findEntityNulls", "FindEntity nulls", { "callerTag", "entityId" })
            renderLedgerArray(state.sidecar, "tweakDbLookups", "TweakDB lookups", { "recordName", "foundRecord" })
            renderLedgerArray(state.sidecar, "statusEffectChanges", "Status-effect changes", { "op", "recordName", "callerTag" })
            renderConflictsPanel(state.sidecar)
            renderHistory(state.result, state.grouping)
            ImGui.Separator()
            if ImGui.Button("Copy summary") then
                ImGui.SetClipboardText(buildSummary(state.result, state.sidecar))
            end
            ImGui.SameLine()
            if ImGui.Button("Copy full report") then
                local ok, enc = pcall(json.encode, state.sidecar)
                if ok and type(enc) == "string" then ImGui.SetClipboardText(enc) end
            end
            ImGui.SameLine()
            if ImGui.Button("Refresh") then refresh() end
            ImGui.Separator()
            wrappedCol(COL_DIM, "These are leads from an automated scan, not a verdict. Verify before changing your load order. REDscope can miss causes a human would catch.")
        else
            renderError(state.err)
            if ImGui.Button("Refresh") then refresh() end
        end
        renderDiagnostic()
        renderSymbols()
    end
    ImGui.End()
end

return Panel
