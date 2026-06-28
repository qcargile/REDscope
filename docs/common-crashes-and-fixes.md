# Common crashes & fixes

Every crash REDscope catches is matched against a library of known crash classes. When the report names one of these, this page tells you what it means, how to spot it in your own `.crash` file, and what to actually do about it.

Each entry corresponds to one signature in REDscope's rule engine. The library is organized by **verdict** — REDscope's best guess at *who* is responsible:

- **Environmental** — your hardware, drivers, or system, not a mod.
- **Memory** — something corrupted memory; the crash site is rarely the culprit.
- **Engine** — a fault inside the game's own code, usually provoked by mod data.
- **Mod** — a specific mod-touched surface is the likely cause.
- **Unknown** — REDscope can't assign blame from the signature alone; the report narrows it for you.

Two things to know before you read on:

- **The faulting address is often *not* the culprit.** For memory-corruption and stack-overflow crashes especially, the place the game noticed the problem is downstream of where the problem started. The verdict and the surrounding state matter more than the crash address.
- **`Cyberpunk2077.exe + 0xOFFSET` is normal.** Only ~84 engine functions are named, so most game frames show as a raw offset. The signal is *which module* faulted and *which scripted call* was in flight — not a named function.

---

## Environmental

These are hardware, driver, or system conditions. A mod can make them fire *more often* (more textures = more VRAM pressure), but it isn't corrupting anything — fix the environment, not the modlist.

### Out of video memory (VRAM exhaustion)

**What it means:** Video memory was at or near its budget when the crash fired. The most likely cause is texture-heavy content the GPU cannot hold.

**How to recognize it:** REDscope flags VRAM exhaustion from the live engine-state read — the report's OOM heuristic is set with a VRAM basis (live GPU memory used vs. budget at crash time). This is a high-confidence call when it fires.

**Fix:**
- Lower the in-game texture quality.
- Reduce 4K/8K texture mods.
- Make sure no other GPU-heavy app is running alongside the game.

### Out of system memory (process commit exhaustion)

**What it means:** System committed memory was at or near its limit when the crash fired. Either the modlist is too memory-heavy for your available RAM + pagefile, or another app is eating committed memory.

**How to recognize it:** REDscope's OOM heuristic fires with a *commit* basis (rather than VRAM). High confidence when it triggers.

**Fix:**
- Reduce the modlist's memory footprint — especially archive-heavy or scripted-heavy mods.
- Close background apps.
- Check your system pagefile size.

### Out of memory / VRAM exhaustion suspected

**What it means:** Memory or VRAM was near-exhausted at crash time, but REDscope couldn't pin down which. This is the general low-confidence fallback for an out-of-memory condition — not necessarily any specific mod.

**How to recognize it:** The report says memory/VRAM was near-exhausted but does **not** carry a specific VRAM or commit basis. If REDscope had a definite basis, it would have shown the VRAM or commit entry above instead (this rule is suppressed whenever one of those fires).

**Fix:**
- Lower texture and streaming settings.
- Reduce the modlist's memory footprint.
- Check your available system RAM and VRAM headroom.

### GPU driver crash / TDR (device removed or reset)

**What it means:** The GPU was removed or reset — a Timeout Detection and Recovery event. This is a driver, hardware, or overclock crash, not a mod.

**How to recognize it:** Look for one of these exception codes in your report:
- `0x887A0005`
- `0x887A0006`
- `0x887A0007`

**Fix:**
- Clean-install your GPU drivers.
- Revert any GPU overclock.
- Check thermals and power delivery.

### Crash inside the GPU usermode driver

**What it means:** The crash faulted inside the GPU usermode driver itself. This is a driver/hardware crash, not a mod — though heavy GPU content can trigger it more often.

**How to recognize it:** The faulting module is one of the GPU driver DLLs:
- NVIDIA: `nvwgf2umx.dll`, `nvd3dumx.dll`
- AMD: `atidxx64.dll`, `amdxc64.dll`
- Intel: `igdumdim64.dll`, `igd10iumd64.dll`

The report shows your GPU driver version.

**Fix:**
- Clean-install or roll back the GPU driver.
- Revert any overclock; check thermals and power.
- Lower GPU-heavy settings.
- If it only happens with certain mods loaded, those mods are *stressing* the driver, not corrupting it.

### Crash inside a DLSS / FSR / XeSS upscaler or frame-gen DLL

**What it means:** The crash faulted inside an upscaler or frame-generation DLL — DLSS, DLSS-G Frame Generation, Ray Reconstruction, FSR, or XeSS. That's the GPU render pipeline, not a mod — though a mod that pushes the GPU harder (heavy textures, extra effects) can make it fire more often.

**How to recognize it:** The faulting module is one of the upscaler/frame-gen DLLs:
- NVIDIA DLSS family: `nvngx_dlss.dll`, `nvngx_dlssg.dll` (Frame Gen), `nvngx_dlssd.dll` (Ray Reconstruction), `sl.interposer.dll`
- Intel XeSS: `libxess.dll`, `libxess_fg.dll`, `libxell.dll`
- AMD FSR: `ffx_fsr3upscaler_x64.dll`, `ffx_frameinterpolation_x64.dll`, `ffx_opticalflow_x64.dll`, `ffx_fsr3_x64.dll`, `amd_ags_x64.dll`

The report shows your live render settings and GPU driver.

**Fix:**
- Toggle DLSS / Frame Generation / Ray Reconstruction off.
- Lower the internal resolution.
- Cap the framerate.
- Update or roll back the GPU driver.
- If it only happens with certain mods loaded, test those — when this crash repeats, the report shows recurrence and co-occurring mods.

### A required framework is not loaded

**What it means:** Installed mods need a framework — Cyber Engine Tweaks or TweakXL — that isn't loaded. Those mods do nothing, and the mismatch can crash the game. This is common after a game update breaks a framework. It's a setup problem, not any single mod's fault.

**How to recognize it:** REDscope's setup-integrity check flags a missing framework. The `setupIntegrity` section of the report names which one.

**Fix:**
- Reinstall or update the named framework.
- Confirm it actually loads — check the red4ext and CET logs at startup.
- See the `setupIntegrity` section for which framework is missing.

---

## Memory

Something wrote where it shouldn't have. For all three of these, **the crash address is where the damage was *noticed*, not where it was *caused*.** The faulting module is usually innocent.

### Heap corruption

**What it means:** Heap corruption was detected. The real culprit corrupted memory earlier in the session; the crash address is only where the engine finally tripped over it — so the faulting module is **not** the suspect.

**How to recognize it:** Exception code `0xC0000374`.

**Fix:**
- Bisect mods that touch native code or memory.
- Check the breadcrumb tail and the loaded-mod set for recently-active mods (the culprit was busy *before* the crash, not at it).

### Stack buffer overrun (/GS check failed)

**What it means:** A stack buffer overrun tripped the `/GS` security cookie — the compiler's stack-smashing guard. Unlike heap corruption, the offending function *is* nearby.

**How to recognize it:** Exception code `0xC0000409`.

**Fix:**
- Identify the module owning the frames near the fault.
- If it's a mod, disable it.

### In-page error (backing store read failed)

**What it means:** A memory page failed to load from its backing store. Possible causes: a bad disk sector, a disconnected drive, or a corrupted memory-mapped file.

**How to recognize it:** Exception code `0xC0000006`. Medium confidence — this can point at hardware/disk as much as software.

**Fix:**
- Verify game file integrity.
- Check the drive the game is installed on.
- Retry after a clean reboot.

---

## Engine

A fault inside the game's own code. These are usually *provoked* by mod data even though the fault is in vanilla.

### Stack overflow / infinite recursion

**What it means:** Infinite recursion or excessive call depth blew the stack. The faulting frame is the symptom site, not the cause — the real signal is the *repeating* call chain.

**How to recognize it:** Exception code `0xC00000FD`. Then look at the native stack for **repeating frames** — the same call chain over and over is the recursion.

**Fix:**
- Identify the repeating call chain.
- If a mod's frames repeat in that chain, disable that mod.

### ICU tooltip-format crash (text formatter on a scripted thread)

**What it means:** The crash faulted inside the ICU Unicode text formatter while a scripted call was in flight. This is almost always a buffer overrun caused by a custom field on a vanilla data class that a scripted handler then asks vanilla code to format — for example, a mod adds float values to a record where vanilla expects none, and a tooltip path then tries to format them.

**How to recognize it:** The faulting module is `icuuc.dll` or `icuin.dll`, **and** the crash is on a scripted thread.

**Fix:**
- Investigate the innermost scripted frame and the mod whose package the script is touching.
- If a recently-added mod started feeding custom fields into vanilla records, that's your lead.

---

## Mod

REDscope is pointing at a specific mod-touched surface.

### Crash on item hover / tooltip resolution

**What it means:** The crash fired on the item-hover / tooltip surface. The most common causes: a broken TweakDB record lookup, a missing ResourcePath/icon CName, a NULL `itemRecord` dereference, or a mod adding fields to a record that vanilla can't format.

**How to recognize it:** The crash is on a scripted thread, and one of these scripted frames is in the stack:
- `OnItemDisplayHoverOver`
- `ShowTooltipsForItemData`
- `RequestItemInspected`
- `OnTooltipShow`
- `OnItemHoverOver`

**Fix:**
- Open inventory and hover items one category at a time to narrow the offending item.
- Disable mods that touch item TweakDB records or tooltip injectors.

---

## Unknown

REDscope can't assign blame from the signature alone — but the report still narrows the search for you.

### Crash during loading (engine initialization)

**What it means:** The crash fired while the engine was still initializing — before gameplay started. Init-time crashes come from mod load order or broken plugin DLLs, not gameplay state.

**How to recognize it:** The engine lifecycle state at crash is one of:
- `BaseInitialization`
- `Initialization`
- `EarlyInitialization`
- `PostInitialization`

**Fix:**
- Bisect the load order: disable the bottom half of the mod list, then narrow down.
- Check `red4ext.log` and the CET load log for plugin-load failures around the crash time.

### Game freeze / hang (no exception)

**What it means:** The game stopped responding *without* an exception — a freeze or deadlock, detected by REDscope's watchdog after no scripted activity for the timeout window. A hang produces no fault, so there's no faulting address; the scripted functions that were last running (the leads) are where to look.

**How to recognize it:** The crash class is `Hang`. There will be **no faulting address** — instead, look at the last-running scripted functions named in the report's leads.

**Fix:**
- If a specific mod's scripted function is named in the leads **and recurs across multiple hangs**, disable that mod.
- Freezes during streaming or loading can also be environmental — disk stalls or GPU driver hangs.

---

*This page mirrors REDscope's signature library (`analyzer/rules/engine-signatures.json`). When the library gains a new crash class, this page should gain an entry.*
