local Grouping = {}

local FRAMEWORK_DLLS = {
  ["redscope.dll"] = true, ["red4ext.dll"] = true,
  ["cyber_engine_tweaks.asi"] = true, ["scc.dll"] = true,
}

local function lc(s)
  if s == nil then return "" end
  return string.lower(tostring(s))
end

local function arr(x)
  if type(x) == "table" then return x end
  return {}
end

local function topModule(summary)
  local fm = summary.faultingModule
  if fm ~= nil and fm ~= "" then return fm end
  local best, bestHits = nil, -1
  for _, sm in ipairs(arr(summary.stackModules)) do
    if sm and sm.name and lc(sm.kind) == "mod" then
      local h = tonumber(sm.hits) or 0
      if h > bestHits then best = sm.name; bestHits = h end
    end
  end
  if best ~= nil then return best end
  for _, sm in ipairs(arr(summary.stackModules)) do
    if sm and sm.name then
      local h = tonumber(sm.hits) or 0
      if h > bestHits then best = sm.name; bestHits = h end
    end
  end
  return best
end

local function labelFor(summary)
  local class = summary.crashClass
  if class == nil or class == "" then class = "Crash" end
  local mod = topModule(summary)
  if mod ~= nil and mod ~= "" then
    return tostring(class) .. " in " .. tostring(mod)
  end
  return tostring(class)
end

function Grouping.analyze(summaries)
  local groups = {}
  local order = {}
  local looseIdToCrashIds = {}
  local moduleToCrashIds = {}
  local total = 0

  for _, s in ipairs(arr(summaries)) do
    total = total + 1
    local id = s.crashId
    if id == nil or id == "" then id = "?" end
    local g = groups[id]
    if g == nil then
      g = {
        crashId = id, looseId = s.looseId, count = 0,
        firstSeen = nil, lastSeen = nil,
        label = labelFor(s), topModule = topModule(s),
        faultSiteUnreliable = s.faultSiteUnreliable == true,
      }
      groups[id] = g
      order[#order + 1] = id
    end
    g.count = g.count + 1
    local t = tonumber(s.crashTimeUnix) or 0
    if g.firstSeen == nil or t < g.firstSeen then g.firstSeen = t end
    if g.lastSeen == nil or t > g.lastSeen then g.lastSeen = t end

    local lid = s.looseId
    if lid ~= nil and lid ~= "" then
      local lset = looseIdToCrashIds[lid]
      if lset == nil then lset = {}; looseIdToCrashIds[lid] = lset end
      lset[id] = true
    end

    for _, sm in ipairs(arr(s.stackModules)) do
      if sm and sm.name then
        local mk = lc(sm.name)
        if not FRAMEWORK_DLLS[mk] then
          local set = moduleToCrashIds[mk]
          if set == nil then set = {}; moduleToCrashIds[mk] = set end
          set[id] = true
        end
      end
    end
  end

  local list = {}
  for _, id in ipairs(order) do list[#list + 1] = groups[id] end
  table.sort(list, function(a, b)
    if a.count ~= b.count then return a.count > b.count end
    local la, lb = a.lastSeen or 0, b.lastSeen or 0
    if la ~= lb then return la > lb end
    return tostring(a.crashId) < tostring(b.crashId)
  end)

  local cooc = {}
  for mk, set in pairs(moduleToCrashIds) do
    local n = 0
    for _ in pairs(set) do n = n + 1 end
    cooc[mk] = n
  end

  local looseDistinct = {}
  for lid, set in pairs(looseIdToCrashIds) do
    local n = 0
    for _ in pairs(set) do n = n + 1 end
    looseDistinct[lid] = n
  end

  return {
    distinctCount = #order,
    totalCount = total,
    groups = list,
    byCrashId = groups,
    looseCounts = looseDistinct,
    cooccurrence = cooc,
  }
end

return Grouping
