local Analyzer = {}

local CONFIDENCE_RANK = { high = 3, medium = 2, low = 1 }

local FRAMEWORK_DLLS = {
  ["redscope.dll"] = true, ["red4ext.dll"] = true,
  ["cyber_engine_tweaks.asi"] = true, ["scc.dll"] = true,
}

local VALID_CONDITIONS = {
  minSchema = true, exceptionCodeAny = true, crashClassAny = true, faultingModuleAny = true,
  stackModuleAll = true, stackModuleAny = true, stackModuleNone = true,
  scriptedFrameAll = true, scriptedFrameAny = true, scriptedFrameNone = true,
  installedModAny = true, installedModVersionMatches = true,
  oomSuspected = true, oomBasisContains = true, engineStateAny = true,
  tweakDbRecordAny = true, statusEffectRecordAny = true,
  findEntityNullCallerAny = true, recentDelayCallbackAny = true,
  wrapLayerModAny = true,
  inFlightClassAny = true,
  curatedConflictActive = true, conflictGroupAny = true,
  setupHasIssues = true, setupIssueKindAny = true,
}
local VALID_VERDICTS = { environmental = true, mod = true, engine = true, memory = true, unknown = true }
local VALID_CONFIDENCE = { high = true, medium = true, low = true }

local function lc(s)
  if s == nil then return "" end
  return string.lower(tostring(s))
end

local function arr(x)
  if type(x) == "table" then return x end
  return {}
end

local function contains(list, value)
  local v = lc(value)
  for _, x in ipairs(list) do if x == v then return true end end
  return false
end

local function anySubstring(haystacks, needle)
  local n = lc(needle)
  for _, h in ipairs(haystacks) do
    if string.find(h, n, 1, true) then return true end
  end
  return false
end

local function allOf(values, pred)
  for _, v in ipairs(values) do if not pred(v) then return false end end
  return true
end

local function anyOf(values, pred)
  for _, v in ipairs(values) do if pred(v) then return true end end
  return false
end

local function stackModuleNames(sidecar)
  local out = {}
  for _, m in ipairs(arr(sidecar.stackModules)) do out[#out + 1] = lc(m and m.name) end
  return out
end

local function scriptedFrameList(sidecar)
  local out = {}
  for _, f in ipairs(arr(sidecar.scriptedFrames)) do out[#out + 1] = lc(f) end
  return out
end

function Analyzer.matchRule(rule, sidecar)
  local m = rule and rule.match
  if not m then return false end

  if m.minSchema ~= nil and not (tonumber(sidecar.schema or 0) >= tonumber(m.minSchema)) then return false end

  if m.exceptionCodeAny then
    local code = lc(sidecar.exception and sidecar.exception.code)
    if not anyOf(arr(m.exceptionCodeAny), function(c) return lc(c) == code end) then return false end
  end
  if m.crashClassAny then
    local cc = lc(sidecar.crashClass)
    if not anyOf(arr(m.crashClassAny), function(c) return lc(c) == cc end) then return false end
  end
  if m.faultingModuleAny then
    local fm = sidecar.faultingModule
    local name = fm and lc(fm.name) or nil
    if name == nil or not anyOf(arr(m.faultingModuleAny), function(x) return lc(x) == name end) then return false end
  end
  if m.stackModuleAll or m.stackModuleAny or m.stackModuleNone then
    local mods = stackModuleNames(sidecar)
    local has = function(x) return contains(mods, x) end
    if m.stackModuleAll and not allOf(arr(m.stackModuleAll), has) then return false end
    if m.stackModuleAny and not anyOf(arr(m.stackModuleAny), has) then return false end
    if m.stackModuleNone and anyOf(arr(m.stackModuleNone), has) then return false end
  end
  if m.scriptedFrameAll or m.scriptedFrameAny or m.scriptedFrameNone then
    local frames = scriptedFrameList(sidecar)
    local has = function(sub) return anySubstring(frames, sub) end
    if m.scriptedFrameAll and not allOf(arr(m.scriptedFrameAll), has) then return false end
    if m.scriptedFrameAny and not anyOf(arr(m.scriptedFrameAny), has) then return false end
    if m.scriptedFrameNone and anyOf(arr(m.scriptedFrameNone), has) then return false end
  end
  if m.installedModAny then
    local names = {}
    for _, x in ipairs(arr(sidecar.installedMods)) do names[#names + 1] = lc(x and x.name) end
    if not anyOf(arr(m.installedModAny), function(x) return contains(names, x) end) then return false end
  end
  if m.installedModVersionMatches then
    local installed = arr(sidecar.installedMods)
    local ok = anyOf(arr(m.installedModVersionMatches), function(needle)
      local target = lc(needle and needle.name)
      local ver = lc(needle and needle.version)
      if target == "" or ver == "" then return false end
      for _, im in ipairs(installed) do
        if lc(im and im.name) == target and lc(im and im.version) == ver then return true end
      end
      return false
    end)
    if not ok then return false end
  end
  if m.oomSuspected ~= nil then
    local oom = sidecar.engineState ~= nil and sidecar.engineState.oomSuspected == true
    local want = m.oomSuspected == true
    if oom ~= want then return false end
  end
  if m.oomBasisContains then
    local basis = lc(sidecar.engineState and sidecar.engineState.oomBasis)
    if basis == "" then return false end
    if not anyOf(arr(m.oomBasisContains), function(s) return string.find(basis, lc(s), 1, true) ~= nil end) then return false end
  end
  if m.engineStateAny then
    local st = lc(sidecar.engineState and sidecar.engineState.state)
    if not anyOf(arr(m.engineStateAny), function(x) return lc(x) == st end) then return false end
  end
  if m.tweakDbRecordAny then
    local lookups = {}
    for _, e in ipairs(arr(sidecar.tweakDbLookups)) do lookups[#lookups + 1] = lc(e and e.recordName) end
    if not anyOf(arr(m.tweakDbRecordAny), function(x) return contains(lookups, x) end) then return false end
  end
  if m.statusEffectRecordAny then
    local recs = {}
    for _, e in ipairs(arr(sidecar.statusEffectChanges)) do recs[#recs + 1] = lc(e and e.recordName) end
    if not anyOf(arr(m.statusEffectRecordAny), function(x) return contains(recs, x) end) then return false end
  end
  if m.findEntityNullCallerAny then
    local callers = {}
    for _, e in ipairs(arr(sidecar.findEntityNulls)) do callers[#callers + 1] = lc(e and e.callerTag) end
    if not anyOf(arr(m.findEntityNullCallerAny), function(sub) return anySubstring(callers, sub) end) then return false end
  end
  if m.recentDelayCallbackAny then
    local cbs = {}
    for _, e in ipairs(arr(sidecar.recentDelays)) do cbs[#cbs + 1] = lc(e and e.callbackClass) end
    if not anyOf(arr(m.recentDelayCallbackAny), function(sub) return anySubstring(cbs, sub) end) then return false end
  end
  if m.wrapLayerModAny then
    local mods = {}
    for _, ch in ipairs(arr(sidecar.wrapChains)) do
      for _, l in ipairs(arr(ch and ch.layers)) do mods[#mods + 1] = lc(l and l.modName) end
    end
    if not anyOf(arr(m.wrapLayerModAny), function(x) return contains(mods, x) end) then return false end
  end
  if m.inFlightClassAny then
    local classes = {}
    for _, o in ipairs(arr(sidecar.objectsInFlight)) do classes[#classes + 1] = lc(o and o.className) end
    if not anyOf(arr(m.inFlightClassAny), function(x) return contains(classes, x) end) then return false end
  end
  if m.curatedConflictActive ~= nil then
    local ac = sidecar.archiveConflicts
    local active = (type(ac) == "table" and type(ac.curated) == "table") and ac.curated.active or {}
    local has = #arr(active) > 0
    if has ~= (m.curatedConflictActive == true) then return false end
  end
  if m.conflictGroupAny then
    local ac = sidecar.archiveConflicts
    local active = (type(ac) == "table" and type(ac.curated) == "table") and ac.curated.active or {}
    local mods = {}
    for _, g in ipairs(arr(active)) do
      for _, mn in ipairs(arr(g and g.mods)) do mods[#mods + 1] = lc(mn) end
    end
    if not anyOf(arr(m.conflictGroupAny), function(x) return contains(mods, x) end) then return false end
  end
  if m.setupHasIssues ~= nil then
    local si = sidecar.setupIntegrity
    local issues = (type(si) == "table") and si.issues or {}
    local has = #arr(issues) > 0
    if has ~= (m.setupHasIssues == true) then return false end
  end
  if m.setupIssueKindAny then
    local si = sidecar.setupIntegrity
    local kinds = {}
    for _, iss in ipairs(arr(si and si.issues)) do kinds[#kinds + 1] = lc(iss and iss.kind) end
    if not anyOf(arr(m.setupIssueKindAny), function(x) return contains(kinds, x) end) then return false end
  end
  return true
end

local function matchLess(a, b)
  local sa, sb = (a.rule.severity or 0), (b.rule.severity or 0)
  if sa ~= sb then return sa > sb end
  local ca, cb = (CONFIDENCE_RANK[a.rule.confidence] or 0), (CONFIDENCE_RANK[b.rule.confidence] or 0)
  if ca ~= cb then return ca > cb end
  return a.ord < b.ord
end

local function evidenceLeads(sidecar)
  local leads = {}
  local seen = {}
  local stackByName = {}
  for _, sm in ipairs(arr(sidecar.stackModules)) do
    if sm and sm.name then stackByName[lc(sm.name)] = sm end
  end
  local function push(name, hits, why)
    local k = lc(name)
    if name == nil or name == "" or seen[k] then return end
    seen[k] = true
    leads[#leads + 1] = { module = name, hits = hits or 0, why = why }
  end
  local fm = sidecar.faultingModule
  if fm and fm.name then
    local sm = stackByName[lc(fm.name)]
    if sm and lc(sm.kind) == "mod" then push(fm.name, sm.hits, "fault inside a mod module") end
  end
  for _, sm in ipairs(arr(sidecar.stackModules)) do
    if sm and lc(sm.kind) == "mod" and not FRAMEWORK_DLLS[lc(sm.name)] then
      push(sm.name, sm.hits, "mod code present in crash stack")
    end
  end
  return leads
end

function Analyzer.analyze(sidecar, ruleSets)
  local rules = {}
  for _, set in ipairs(arr(ruleSets)) do
    for _, r in ipairs(arr(set)) do rules[#rules + 1] = r end
  end
  local matched = {}
  local matchedIds = {}
  for i, r in ipairs(rules) do
    if Analyzer.matchRule(r, sidecar) then
      matched[#matched + 1] = { rule = r, ord = i }
      if r.id ~= nil then matchedIds[r.id] = true end
    end
  end
  local kept = {}
  for _, m in ipairs(matched) do
    local suppressed = false
    for _, sid in ipairs(arr(m.rule.suppressedBy)) do
      if matchedIds[sid] then suppressed = true break end
    end
    if not suppressed then kept[#kept + 1] = m end
  end
  table.sort(kept, matchLess)
  local matches = {}
  for _, m in ipairs(kept) do
    local r = m.rule
    matches[#matches + 1] = {
      id = r.id, name = r.name, severity = r.severity, verdict = r.verdict,
      confidence = r.confidence, message = r.message, fix = r.fix, links = r.links or {},
    }
  end
  local fm = sidecar.faultingModule
  return {
    schema = sidecar.schema,
    crashId = sidecar.crashId,
    facts = {
      exceptionCode = sidecar.exception and sidecar.exception.code or nil,
      faultingModule = fm and fm.name or nil,
      crashClass = sidecar.crashClass,
    },
    matches = matches,
    topMatch = matches[1] or nil,
    leads = evidenceLeads(sidecar),
  }
end

function Analyzer.validateRule(rule)
  local errs = {}
  if type(rule) ~= "table" then return { "rule is not a table" } end
  local id = rule.id or "(no id)"
  if type(rule.id) ~= "string" then errs[#errs + 1] = "missing string id" end
  if type(rule.severity) ~= "number" or rule.severity < 1 or rule.severity > 6 then
    errs[#errs + 1] = id .. ": severity must be an integer 1-6"
  end
  if not VALID_VERDICTS[rule.verdict] then errs[#errs + 1] = id .. ": invalid verdict \"" .. tostring(rule.verdict) .. "\"" end
  if not VALID_CONFIDENCE[rule.confidence] then errs[#errs + 1] = id .. ": invalid confidence \"" .. tostring(rule.confidence) .. "\"" end
  local m = rule.match
  local hasCond = false
  if type(m) == "table" then for _ in pairs(m) do hasCond = true break end end
  if type(m) ~= "table" or not hasCond then
    errs[#errs + 1] = id .. ": match must be a table with at least one condition"
  else
    for k in pairs(m) do
      if not VALID_CONDITIONS[k] then errs[#errs + 1] = id .. ": unknown match condition " .. tostring(k) end
    end
  end
  if type(rule.message) ~= "string" then errs[#errs + 1] = id .. ": missing string message" end
  return errs
end

return Analyzer
