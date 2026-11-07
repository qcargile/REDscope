local Parser = {}

local function trim(s)
  if s == nil then return "" end
  return tostring(s):match("^%s*(.-)%s*$") or ""
end

local function nilIfEmpty(s)
  if s == nil or s == "" then return nil end
  return s
end

local function splitLines(text)
  local out = {}
  if text == nil then return out end
  for line in tostring(text):gmatch("([^\r\n]*)\r?\n?") do
    out[#out + 1] = line
  end
  if #out > 0 and out[#out] == "" then out[#out] = nil end
  return out
end

local function sectionLabel(line)
  return line:match("^%-%-%-%s+(.-)%s+%-+%s*$")
end

local function startsWith(s, prefix)
  return s:sub(1, #prefix) == prefix
end

local kFrameworkDlls = {
  ["redscope.dll"]=true, ["red4ext.dll"]=true,
  ["cyber_engine_tweaks.asi"]=true, ["scc.dll"]=true,
}

local function classifyStackKind(moduleName)
  if moduleName == nil or moduleName == "" then return "unknown" end
  local n = string.lower(moduleName)
  if n == "cyberpunk2077.exe" then return "game" end
  if kFrameworkDlls[n] then return "framework" end
  local sys = {
    ["ntdll.dll"]=true, ["kernel32.dll"]=true, ["kernelbase.dll"]=true,
    ["user32.dll"]=true, ["ucrtbase.dll"]=true, ["msvcp140.dll"]=true,
    ["vcruntime140.dll"]=true, ["vcruntime140_1.dll"]=true,
    ["gdi32.dll"]=true, ["gdi32full.dll"]=true, ["combase.dll"]=true,
    ["ole32.dll"]=true, ["oleaut32.dll"]=true, ["shell32.dll"]=true,
    ["advapi32.dll"]=true, ["ws2_32.dll"]=true, ["d3d12.dll"]=true,
    ["dxgi.dll"]=true, ["d3d11.dll"]=true, ["dbghelp.dll"]=true,
    ["msvcrt.dll"]=true, ["rpcrt4.dll"]=true, ["sechost.dll"]=true,
    ["bcrypt.dll"]=true, ["bcryptprimitives.dll"]=true, ["win32u.dll"]=true,
  }
  if sys[n] then return "system" end
  return "mod"
end

local function splitSections(lines)
  local sections = { header = {} }
  local order = { "header" }
  local current = "header"
  for _, line in ipairs(lines) do
    local label = sectionLabel(line)
    if label ~= nil then
      current = label
      if sections[current] == nil then
        sections[current] = {}
        order[#order + 1] = current
      end
    else
      table.insert(sections[current], line)
    end
  end
  return sections, order
end

local function parseHeader(lines, out)
  for _, line in ipairs(lines) do
    local culprit = line:match("^LIKELY CULPRIT:%s*(.*)$")
    if culprit then out.likelyCulprit = trim(culprit) end

    local human, loose = line:match("^CRASH ID:%s*(%S+)%s*%(loose%s+(%S+)%)")
    if human then
      out.crashId = trim(human)
      out.looseId = trim(loose)
    end

    local fpStrict, fpLoose, modset = line:match("fingerprint%s+(%x+)%s*/%s*(%x+)%s+modset%s+(%x+)")
    if fpStrict then
      out.fingerprint = { primary = fpStrict, loose = fpLoose, modSet = modset }
    end
  end
end

local function parseCrashSection(lines, out)
  for _, line in ipairs(lines) do
    local excCode, excName = line:match("^Exception%s*:%s*(0x%x+)%s*(.*)$")
    if excCode then
      out.exception = out.exception or {}
      out.exception.code = excCode
      out.exception.name = trim(excName)
    end

    local addr = line:match("^Address%s*:%s*(0x%x+)")
    if addr then
      out.exception = out.exception or {}
      out.exception.address = addr
    end

    local faultOp, faultAddr = line:match("^Fault data:%s+(%S+)%s+at%s+(0x%x+)")
    if faultOp then
      out.faultData = { op = faultOp, address = faultAddr }
    end

    local modName, modOff = line:match("^%s+module:%s+(%S+)%s*%+%s*(0x%x+)")
    if modName then
      out.faultingModule = { name = modName, rva = modOff }
    end

    local timeStr = line:match("^Time%s*:%s*(.*)$")
    if timeStr then out.timeText = trim(timeStr) end

    local className = line:match("^Class%s*:%s*(.*)$")
    if className then out.crashClass = trim(className) end

    local rsv = line:match("^REDscope%s*:%s*(.*)$")
    if rsv then out.redscopeVersion = trim(rsv) end

    local cpv = line:match("^Cyberpunk%s*:%s*(.*)$")
    if cpv then out.buildId = trim(cpv) end
  end
end

local function parseLastActive(lines, out)
  out.lastActive = out.lastActive or {}
  out.lastActive.scriptedThreads = {}
  out.lastActive.modCodeOnStack = {}
  out.lastActive.liveGameState = {}
  local mode = nil
  for _, line in ipairs(lines) do
    local sub = trim(line)
    if sub:match("^Scripted code") then
      mode = "scripted"
    elseif sub:match("^Live game state") then
      mode = "gameState"
    elseif sub:match("^Mod code on native stack") then
      mode = "modcode"
    elseif sub:match("^Mods changed since last launch") then
      mode = "mods"
    elseif mode == "scripted" then
      local isCrashing = true
      local tid, name, secs = line:match("^%s*%[CRASHING%]%s*tid=(%d+)%s+(%S.-)%s+%(%s*([%+%-]?[%d%.]+)s%s+ago%)")
      if tid == nil then
        isCrashing = false
        tid, name, secs = line:match("^%s*tid=(%d+)%s+(%S.-)%s+%(%s*([%+%-]?[%d%.]+)s%s+ago%)")
      end
      if tid and name then
        table.insert(out.lastActive.scriptedThreads, {
          isCrashing = isCrashing,
          tid = tonumber(tid),
          name = trim(name),
          secondsAgo = tonumber(secs),
        })
      end
    elseif mode == "gameState" then
      local k, v = line:match("^%s+(%S+)%s+(.-)%s*$")
      if k and v ~= "" then
        out.lastActive.liveGameState[k] = trim(v)
      end
    elseif mode == "modcode" then
      local idx, pc, mod, off = line:match("^%s*%[%s*(%d+)%]%s+(0x%x+)%s+(%S+)%s+%+%s+(0x%x+)")
      if idx then
        table.insert(out.lastActive.modCodeOnStack, {
          frameIdx = tonumber(idx),
          pc = pc,
          module = mod,
          offset = off,
        })
      end
    end
  end
end

local function parseNativeCallstack(lines, out)
  for _, line in ipairs(lines) do
    local addr, mod, rva = line:match("^%[%s*0%]%s+(0x%x+)%s+(%S-)!.-%+(0x%x+)%s+<faulting RIP>")
    if addr == nil then
      addr, mod, rva = line:match("^%[%s*0%]%s+(0x%x+)%s+(%S-)%s+%+%s+(0x%x+)%s+<faulting RIP>")
    end
    if addr then
      out.faultingModule = out.faultingModule or { name = mod, rva = rva }
      out.exception = out.exception or {}
      if out.exception.address == nil then out.exception.address = addr end
      return
    end
  end
end

local function parseRedScriptCallStack(lines, out)
  out.redScriptStack = {}
  for _, line in ipairs(lines) do
    local name = line:match("^%[%s*%d+%]%s+(%S.-)%s+%(([%+%-]?[%d%.]+)s%s+ago%)")
    if name then table.insert(out.redScriptStack, trim(name)) end
  end
end

local function parseInstalledModsSummary(lines, out)
  for _, line in ipairs(lines) do
    local mgr, rest = line:match("^Manager:%s*(%S+)%s*(.*)$")
    if mgr then
      out.modManager = mgr
      rest = trim(rest)
      if rest ~= "" then
        out.modManagerDetails = mgr .. " " .. rest
      else
        out.modManagerDetails = mgr
      end
    end
    local installed, enabled = line:match("^(%d+)%s+installed,%s+(%d+)%s+enabled")
    if installed then
      out.installedSummary = {
        installed = tonumber(installed),
        enabled = tonumber(enabled),
      }
    end
  end
end

local function parseBreadcrumbs(lines, out)
  out.breadcrumbs = {}
  for _, line in ipairs(lines) do
    local secs, tag, msg = line:match("^%[%s*([%+%-][%d%.]+)s%]%s+%[([^%]]*)%]%s+(.*)$")
    if secs then
      table.insert(out.breadcrumbs, {
        secondsRelToCrash = tonumber(secs),
        tag = trim(tag),
        message = msg,
      })
    end
  end
end

local function parseRegisters(lines, out)
  out.registers = {}
  for _, line in ipairs(lines) do
    local reg, val, annot = line:match("^(R%w%w?)%s+(0x%x+)%s*(.*)$")
    if reg then
      out.registers[trim(reg)] = { value = val, annotation = trim(annot) }
    end
  end
end

local function parseEngineStateInline(lines, out)
  out.engineState = out.engineState or {}
  for _, line in ipairs(lines) do
    local k, v = line:match("^(.-)%s*:%s*(.*)$")
    if k then
      k = trim(k)
      v = trim(v)
      if k == "Engine state" then out.engineState.state = v
      elseif k == "Scripts loaded" then out.engineState.scriptsLoaded = (v == "true")
      elseif k == "Engine closing" then out.engineState.closing = (v:sub(1, 4) == "true")
      elseif k == "OOM suspected" then out.engineState.oomSuspected = (v:sub(1, 4) == "true")
      elseif k == "GPU memory" then
        local used, ded, budget = v:match("(%d+)%s*/%s*(%d+)%s*MB used%s*%(budget%s+(%d+)")
        if used then
          out.engineState.gpuUsedMB = tonumber(used)
          out.engineState.gpuDedicatedMB = tonumber(ded)
          out.engineState.gpuBudgetMB = tonumber(budget)
        end
      end
    end
  end
end

local function parseEngineStateSibling(text, out)
  if text == nil or text == "" then return end
  out.engineState = out.engineState or {}
  for _, line in ipairs(splitLines(text)) do
    local k, v = line:match("^(.-)%s*:%s*(.*)$")
    if k then
      k = trim(k)
      v = trim(v)
      if k == "Loading stage" then out.engineState.loadingStage = v
      elseif k == "World" then out.engineState.world = v
      elseif k == "Is in game" then out.engineState.isInGame = (v == "true")
      elseif k == "uptimeSeconds" then out.engineState.uptimeSeconds = tonumber(v)
      end
    end
  end
end

local function buildStackModules(out)
  out.stackModules = {}
  local counts = {}
  local order = {}
  for _, entry in ipairs(out.lastActive and out.lastActive.modCodeOnStack or {}) do
    local mod = entry.module
    if mod and mod ~= "" then
      if counts[mod] == nil then
        counts[mod] = 0
        order[#order + 1] = mod
      end
      counts[mod] = counts[mod] + 1
    end
  end
  for _, mod in ipairs(order) do
    table.insert(out.stackModules, {
      name = mod,
      hits = counts[mod],
      kind = classifyStackKind(mod),
    })
  end
end

local function buildScriptedFrames(out)
  out.scriptedFrames = {}
  if out.redScriptStack and #out.redScriptStack > 0 then
    for i = #out.redScriptStack, 1, -1 do
      table.insert(out.scriptedFrames, out.redScriptStack[i])
    end
    return
  end
  for _, entry in ipairs(out.lastActive and out.lastActive.scriptedThreads or {}) do
    if entry.isCrashing and entry.name and entry.name ~= "" then
      table.insert(out.scriptedFrames, entry.name)
    end
  end
end

local function buildGameStateLive(out)
  if out.lastActive and out.lastActive.liveGameState then
    local any = false
    for _ in pairs(out.lastActive.liveGameState) do any = true break end
    if any then out.gameStateLive = out.lastActive.liveGameState end
  end
end

local function inferFaultSiteUnreliable(out)
  out.fingerprint = out.fingerprint or {}
  if out.likelyCulprit and out.likelyCulprit:find("does not lie inside any loaded module", 1, true) then
    out.fingerprint.faultSiteUnreliable = true
  else
    out.fingerprint.faultSiteUnreliable = false
  end
end

function Parser.parseCrash(text)
  local out = {
    schema = 3,
    source = "crash-text-fallback",
    warnings = {},
  }
  if text == nil or text == "" then
    table.insert(out.warnings, "empty input")
    return out
  end

  local lines = splitLines(text)
  local sections, order = splitSections(lines)

  parseHeader(sections.header or {}, out)
  if sections["Crash"] then parseCrashSection(sections["Crash"], out) end

  for _, label in ipairs(order) do
    if startsWith(label, "LAST ACTIVE") then
      parseLastActive(sections[label], out)
    elseif startsWith(label, "Breadcrumbs") then
      parseBreadcrumbs(sections[label], out)
    elseif label == "Registers" then
      parseRegisters(sections[label], out)
    elseif startsWith(label, "Engine state") then
      parseEngineStateInline(sections[label], out)
    elseif startsWith(label, "Native call stack") then
      parseNativeCallstack(sections[label], out)
    elseif startsWith(label, "RedScript call stack") then
      parseRedScriptCallStack(sections[label], out)
    elseif startsWith(label, "Installed mods") then
      parseInstalledModsSummary(sections[label], out)
    end
  end

  buildStackModules(out)
  buildScriptedFrames(out)
  buildGameStateLive(out)
  inferFaultSiteUnreliable(out)

  if out.crashId == nil then table.insert(out.warnings, "no crashId extracted") end
  if out.exception == nil then table.insert(out.warnings, "no exception extracted") end

  return out
end

function Parser.parseFromCombined(text)
  if text == nil or text == "" then return nil end
  local crashText = text
  local stateText = nil
  local marker = "\n--- engine-state.txt (sibling) ---\n"
  local pos = text:find(marker, 1, true)
  if pos then
    crashText = text:sub(1, pos - 1)
    stateText = text:sub(pos + #marker)
  end
  local out = Parser.parseCrash(crashText)
  if stateText then parseEngineStateSibling(stateText, out) end
  return out
end

return Parser
