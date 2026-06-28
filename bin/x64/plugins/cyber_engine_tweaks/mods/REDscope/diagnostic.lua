local Diagnostic = {}

local SETTINGS = {
    { "/video/display", "Resolution", "Resolution" },
    { "/video/display", "WindowMode", "Window mode" },
    { "/video/display", "OutputMonitor", "Monitor" },
    { "/video/display", "HDRModes", "HDR" },
    { "/video/display", "VSync", "VSync" },
    { "/video/display", "ReflexMode", "Nvidia Reflex" },
    { "/graphics/presets", "DLSS", "Upscaler" },
    { "/graphics/presets", "DLSS_D", "Ray reconstruction" },
    { "/graphics/presets", "FrameGeneration", "Frame generation" },
    { "/graphics/presets", "DynamicResolutionScaling", "Dynamic resolution" },
    { "/graphics/presets", "TextureQuality", "Texture quality" },
    { "/graphics/raytracing", "RayTracing", "Ray tracing" },
    { "/graphics/raytracing", "RayTracedReflections", "RT reflections" },
    { "/graphics/raytracing", "RayTracedSunShadows", "RT sun shadows" },
    { "/graphics/raytracing", "RayTracedLocalShadows", "RT local shadows" },
    { "/graphics/raytracing", "RayTracedLighting", "RT lighting" },
    { "/graphics/raytracing", "RayTracedPathTracing", "Path tracing" },
    { "/graphics/advanced", "ShadowMeshQuality", "Shadow quality" },
    { "/graphics/advanced", "LocalShadowsQuality", "Local shadow quality" },
    { "/graphics/advanced", "VolumetricFogResolution", "Volumetric fog" },
    { "/graphics/advanced", "VolumetricCloudsQuality", "Volumetric clouds" },
    { "/graphics/advanced", "ScreenSpaceReflectionsQuality", "SSR quality" },
    { "/graphics/advanced", "AmbientOcclusion", "Ambient occlusion" },
    { "/graphics/advanced", "DistantShadowsResolution", "Distant shadows" },
    { "/graphics/advanced", "MaxDynamicDecals", "Dynamic decals" },
    { "/graphics/advanced", "MirrorQuality", "Mirror quality" },
    { "/graphics/advanced", "ContactShadows", "Contact shadows" },
    { "/graphics/advanced", "LODPreset", "Level of detail" },
    { "/graphics/advanced", "AntiAliasing", "Anti-aliasing" },
    { "/graphics/basic", "FieldOfView", "FOV" },
    { "/graphics/basic", "MotionBlur", "Motion blur" },
    { "/graphics/basic", "FilmGrain", "Film grain" },
    { "/graphics/basic", "ChromaticAberration", "Chromatic aberration" },
    { "/graphics/basic", "DepthOfField", "Depth of field" },
    { "/graphics/basic", "LensFlares", "Lens flares" },
    { "/graphics/basic", "Vignette", "Vignette" },
    { "/gameplay/performance", "CrowdDensity", "Crowd density" },
    { "/gameplay/difficulty", "GameDifficulty", "Difficulty" },
}

local function resolveLocKey(s)
    if type(s) ~= "string" or s == "" or s:find(" ") then return s end
    local _, hyphens = s:gsub("%-", "")
    if hyphens < 2 then return s end
    local ok, r = pcall(GetLocalizedTextByKey, s)
    if ok and type(r) == "string" and r ~= "" and r ~= s then return r end
    return s
end

local function valToString(v)
    if v == nil then return nil end
    local t = type(v)
    if t == "boolean" then return v and "On" or "Off" end
    if t == "number" then return tostring(v) end
    if t == "string" then return resolveLocKey(v) end
    local ok, s = pcall(NameToString, v)
    if ok and type(s) == "string" and s ~= "" then return resolveLocKey(s) end
    ok, s = pcall(tostring, v)
    if ok and type(s) == "string" then return resolveLocKey(s) end
    return nil
end

local function readSettingFrom(ss, group, var)
    if ss == nil then return nil end
    local ok, val = pcall(function()
        if not ss:HasVar(group, var) then return nil end
        local cv = ss:GetVar(group, var)
        if cv == nil then return nil end
        return cv:GetValue()
    end)
    if not ok then return nil end
    return valToString(val)
end

function Diagnostic.readSettings()
    local okSys, ss = pcall(function() return Game.GetSettingsSystem() end)
    if not okSys then ss = nil end
    local out = {}
    for _, s in ipairs(SETTINGS) do
        local v = readSettingFrom(ss, s[1], s[2])
        if v ~= nil and v ~= "" then
            out[#out + 1] = { label = s[3], value = v }
        end
    end
    return out
end

local function fetchSnapshot()
    local getter = Game["GetDiagnosticJson"]
    if type(getter) ~= "function" then return nil, "native_unbound" end
    local ok, content = pcall(getter)
    if not ok or type(content) ~= "string" or content == "" then return nil, "native_bad_return" end
    local okDec, data = pcall(json.decode, content)
    if not okDec or type(data) ~= "table" then return nil, "decode_failed" end
    return data, nil
end

local function fmtMB(mb)
    local n = tonumber(mb)
    if n == nil or n <= 0 then return nil end
    if n >= 1024 then return string.format("%.1f GB", n / 1024) end
    return tostring(n) .. " MB"
end

local function appendSpecs(L, data)
    if type(data.buildId) == "string" and data.buildId ~= "" then L[#L + 1] = "Game build: " .. data.buildId end
    if type(data.red4extVersion) == "string" and data.red4extVersion ~= "" then L[#L + 1] = "RED4ext: " .. data.red4extVersion end

    local hw = data.hardware
    if type(hw) == "table" then
        if hw.cpu then
            L[#L + 1] = string.format("CPU: %s (%s cores / %s threads)",
                tostring(hw.cpu), tostring(hw.cpuCores or "?"), tostring(hw.cpuThreads or "?"))
        end
        if hw.gpu then
            local vram = fmtMB(hw.vramMB)
            L[#L + 1] = "GPU: " .. tostring(hw.gpu)
                .. (vram and ("  " .. vram) or "")
                .. (hw.gpuDriver and ("  driver " .. tostring(hw.gpuDriver)) or "")
        end
        local ram = fmtMB(hw.ramTotalMB)
        if ram then
            local extra = ""
            if type(hw.ramType) == "string" and hw.ramType ~= "" then extra = extra .. "  " .. hw.ramType end
            if tonumber(hw.ramSpeedMHz) and hw.ramSpeedMHz > 0 then extra = extra .. "  " .. tostring(hw.ramSpeedMHz) .. " MHz" end
            L[#L + 1] = "RAM: " .. ram .. extra
        end
        if hw.os then
            L[#L + 1] = "OS: " .. tostring(hw.os)
                .. (hw.osVersion and ("  " .. tostring(hw.osVersion)) or "")
                .. (tonumber(hw.osBuild) and hw.osBuild > 0 and ("  build " .. tostring(hw.osBuild)) or "")
        end
        if tonumber(hw.displayW) and hw.displayW > 0 then
            L[#L + 1] = string.format("Display: %sx%s @ %sHz%s",
                tostring(hw.displayW), tostring(hw.displayH), tostring(hw.displayHz),
                (type(hw.displayHdr) == "string" and hw.displayHdr ~= "" and ("  HDR " .. hw.displayHdr) or ""))
        end
    end
end

function Diagnostic.build()
    local data, reason = fetchSnapshot()
    if data == nil then return nil, reason end
    local settings = Diagnostic.readSettings()

    local mods = data.installedMods
    local enabledCount, total = 0, 0
    if type(mods) == "table" then
        total = #mods
        for _, m in ipairs(mods) do if m.enabled then enabledCount = enabledCount + 1 end end
    end
    local mgr = data.modManagerDetails
    if type(mgr) ~= "string" or mgr == "" then mgr = data.modManager or "unknown" end

    local head = {}
    head[#head + 1] = "REDscope diagnostic report"
    head[#head + 1] = "Facts about your setup. Generated in-game."
    head[#head + 1] = ""
    head[#head + 1] = "== System =="
    appendSpecs(head, data)
    head[#head + 1] = ""
    if #settings > 0 then
        head[#head + 1] = "== Settings =="
        for _, s in ipairs(settings) do head[#head + 1] = s.label .. ": " .. s.value end
        head[#head + 1] = ""
    end

    local quick = {}
    for _, line in ipairs(head) do quick[#quick + 1] = line end
    quick[#quick + 1] = "== Mods =="
    quick[#quick + 1] = "Manager: " .. tostring(mgr)
    quick[#quick + 1] = string.format("%d enabled / %d total (full list in the downloaded report)", enabledCount, total)

    local full = {}
    for _, line in ipairs(head) do full[#full + 1] = line end
    full[#full + 1] = string.format("== Mods (%d enabled / %d total) ==", enabledCount, total)
    full[#full + 1] = "Manager: " .. tostring(mgr)
    if type(mods) == "table" then
        for _, m in ipairs(mods) do
            local line = tostring(m.name or "?")
            if type(m.version) == "string" and m.version ~= "" then line = line .. "  " .. m.version end
            if type(m.type) == "string" and m.type ~= "unknown" then line = line .. "  [" .. m.type .. "]" end
            if not m.enabled then line = line .. "  (disabled)" end
            full[#full + 1] = line
        end
    end

    return {
        quick = table.concat(quick, "\n"),
        full = table.concat(full, "\n"),
        enabledCount = enabledCount,
        total = total,
        settingsCount = #settings,
    }
end

function Diagnostic.copy(report)
    if report == nil then return false end
    return pcall(function() ImGui.SetClipboardText(report.quick) end)
end

function Diagnostic.download(report)
    if report == nil then return nil end
    local ts = "report"
    if type(os) == "table" and type(os.date) == "function" then
        local okD, s = pcall(os.date, "%Y%m%d-%H%M%S")
        if okD and type(s) == "string" then ts = s end
    end
    local name = "REDscope-diagnostic-" .. ts .. ".txt"
    local f = io.open(name, "w")
    if f == nil then return nil end
    f:write(report.full)
    f:close()
    return name
end

return Diagnostic
