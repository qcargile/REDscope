const kFrameworkDlls = new Set([
  'redscope.dll', 'red4ext.dll', 'cyber_engine_tweaks.asi', 'scc.dll',
]);

const kSystemDlls = new Set([
  'ntdll.dll', 'kernel32.dll', 'kernelbase.dll', 'user32.dll', 'ucrtbase.dll',
  'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll',
  'gdi32.dll', 'gdi32full.dll', 'combase.dll', 'ole32.dll', 'oleaut32.dll',
  'shell32.dll', 'advapi32.dll', 'ws2_32.dll', 'd3d12.dll', 'dxgi.dll',
  'd3d11.dll', 'dbghelp.dll', 'msvcrt.dll', 'rpcrt4.dll', 'sechost.dll',
  'bcrypt.dll', 'bcryptprimitives.dll', 'win32u.dll',
]);

function classifyStackKind(name) {
  if (!name) return 'unknown';
  const n = name.toLowerCase();
  if (n === 'cyberpunk2077.exe') return 'game';
  if (kFrameworkDlls.has(n)) return 'framework';
  if (kSystemDlls.has(n)) return 'system';
  return 'mod';
}

function trim(s) { return (s == null ? '' : String(s)).replace(/^\s+|\s+$/g, ''); }
function startsWith(s, p) { return s.slice(0, p.length) === p; }

function splitLines(text) {
  if (!text) return [];
  return text.split(/\r?\n/);
}

function sectionLabel(line) {
  const m = line.match(/^---\s+(.*?)\s+-+\s*$/);
  return m ? m[1] : null;
}

function splitSections(lines) {
  const sections = { header: [] };
  const order = ['header'];
  let current = 'header';
  for (const line of lines) {
    const label = sectionLabel(line);
    if (label != null) {
      current = label;
      if (!sections[current]) { sections[current] = []; order.push(current); }
    } else {
      sections[current].push(line);
    }
  }
  return { sections, order };
}

function parseHeader(lines, out) {
  for (const line of lines) {
    const culprit = line.match(/^LIKELY CULPRIT:\s*(.*)$/);
    if (culprit) out.likelyCulprit = trim(culprit[1]);

    const idLine = line.match(/^CRASH ID:\s*(\S+)\s*\(loose\s+(\S+)\)/);
    if (idLine) { out.crashId = trim(idLine[1]); out.looseId = trim(idLine[2]); }

    const fp = line.match(/fingerprint\s+([0-9A-Fa-f]+)\s*\/\s*([0-9A-Fa-f]+)\s+modset\s+([0-9A-Fa-f]+)/);
    if (fp) out.fingerprint = { primary: fp[1], loose: fp[2], modSet: fp[3] };
  }
}

function parseCrashSection(lines, out) {
  for (const line of lines) {
    const exc = line.match(/^Exception\s*:\s*(0x[0-9A-Fa-f]+)\s*(.*)$/);
    if (exc) { out.exception = out.exception || {}; out.exception.code = exc[1]; out.exception.name = trim(exc[2]); }

    const addr = line.match(/^Address\s*:\s*(0x[0-9A-Fa-f]+)/);
    if (addr) { out.exception = out.exception || {}; out.exception.address = addr[1]; }

    const fault = line.match(/^Fault data:\s+(\S+)\s+at\s+(0x[0-9A-Fa-f]+)/);
    if (fault) out.faultData = { op: fault[1], address: fault[2] };

    const mod = line.match(/^\s+module:\s+(\S+)\s*\+\s*(0x[0-9A-Fa-f]+)/);
    if (mod) out.faultingModule = { name: mod[1], rva: mod[2] };

    const timeStr = line.match(/^Time\s*:\s*(.*)$/);
    if (timeStr) out.timeText = trim(timeStr[1]);

    const cls = line.match(/^Class\s*:\s*(.*)$/);
    if (cls) out.crashClass = trim(cls[1]);

    const rsv = line.match(/^REDscope\s*:\s*(.*)$/);
    if (rsv) out.redscopeVersion = trim(rsv[1]);

    const cp = line.match(/^Cyberpunk\s*:\s*(.*)$/);
    if (cp) out.buildId = trim(cp[1]);
  }
}

function parseNativeCallstack(lines, out) {
  for (const line of lines) {
    const m = line.match(/^\[\s*0\]\s+(0x[0-9A-Fa-f]+)\s+(\S+?)!.*?\+(0x[0-9A-Fa-f]+)\s+<faulting RIP>/);
    if (m) {
      out.faultingModule = out.faultingModule || { name: m[2], rva: m[3] };
      out.exception = out.exception || {};
      if (!out.exception.address) out.exception.address = m[1];
      return;
    }
    const bare = line.match(/^\[\s*0\]\s+(0x[0-9A-Fa-f]+)\s+(\S+?)\s+\+\s+(0x[0-9A-Fa-f]+)\s+<faulting RIP>/);
    if (bare) {
      out.faultingModule = out.faultingModule || { name: bare[2], rva: bare[3] };
      out.exception = out.exception || {};
      if (!out.exception.address) out.exception.address = bare[1];
      return;
    }
  }
}

function parseRedScriptCallStack(lines, out) {
  out.redScriptStack = [];
  for (const line of lines) {
    const m = line.match(/^\[\s*\d+\]\s+(\S.*?)\s+\(([+\-]?[\d.]+)s\s+ago\)/);
    if (m) out.redScriptStack.push(trim(m[1]));
  }
}

function parseLastActive(lines, out) {
  out.lastActive = out.lastActive || {};
  out.lastActive.scriptedThreads = [];
  out.lastActive.modCodeOnStack = [];
  out.lastActive.liveGameState = {};
  let mode = null;
  for (const line of lines) {
    const sub = trim(line);
    if (/^Scripted code/.test(sub)) { mode = 'scripted'; continue; }
    if (/^Live game state/.test(sub)) { mode = 'gameState'; continue; }
    if (/^Mod code on native stack/.test(sub)) { mode = 'modcode'; continue; }
    if (/^Mods changed since last launch/.test(sub)) { mode = 'mods'; continue; }
    if (mode === 'scripted') {
      const m = line.match(/^\s*(\[CRASHING\])?\s*tid=(\d+)\s+(\S.*?)\s+\(\s*([+\-]?[\d.]+)s\s+ago\)/);
      if (m) {
        out.lastActive.scriptedThreads.push({
          isCrashing: m[1] != null && m[1] !== '',
          tid: parseInt(m[2], 10),
          name: trim(m[3]),
          secondsAgo: parseFloat(m[4]),
        });
      }
    } else if (mode === 'gameState') {
      const m = line.match(/^\s+(\S+)\s+(.+?)\s*$/);
      if (m) out.lastActive.liveGameState[m[1]] = trim(m[2]);
    } else if (mode === 'modcode') {
      const m = line.match(/^\s*\[\s*(\d+)\]\s+(0x[0-9A-Fa-f]+)\s+(\S+)\s+\+\s+(0x[0-9A-Fa-f]+)/);
      if (m) out.lastActive.modCodeOnStack.push({
        frameIdx: parseInt(m[1], 10), pc: m[2], module: m[3], offset: m[4],
      });
    }
  }
}

function parseInstalledModsSummary(lines, out) {
  for (const line of lines) {
    const mgr = line.match(/^Manager:\s*(\S+)\s*(.*)$/);
    if (mgr) {
      out.modManager = mgr[1];
      const rest = trim(mgr[2]);
      out.modManagerDetails = rest ? `${mgr[1]} ${rest}` : mgr[1];
    }
    const counts = line.match(/^(\d+)\s+installed,\s+(\d+)\s+enabled/);
    if (counts) {
      out.installedSummary = {
        installed: parseInt(counts[1], 10),
        enabled: parseInt(counts[2], 10),
      };
    }
  }
}

function parseEngineStateInline(lines, out) {
  out.engineState = out.engineState || {};
  for (const line of lines) {
    const m = line.match(/^(.+?)\s*:\s*(.*)$/);
    if (!m) continue;
    const k = trim(m[1]);
    const v = trim(m[2]);
    if (k === 'Engine state') out.engineState.state = v;
    else if (k === 'Scripts loaded') out.engineState.scriptsLoaded = v === 'true';
    else if (k === 'Engine closing') out.engineState.closing = v.slice(0, 4) === 'true';
    else if (k === 'OOM suspected') out.engineState.oomSuspected = v.slice(0, 4) === 'true';
    else if (k === 'GPU memory') {
      const g = v.match(/(\d+)\s*\/\s*(\d+)\s*MB used\s*\(budget\s+(\d+)/);
      if (g) {
        out.engineState.gpuUsedMB = parseInt(g[1], 10);
        out.engineState.gpuDedicatedMB = parseInt(g[2], 10);
        out.engineState.gpuBudgetMB = parseInt(g[3], 10);
      }
    }
  }
}

function buildStackModules(out) {
  out.stackModules = [];
  const counts = {};
  const order = [];
  for (const e of (out.lastActive && out.lastActive.modCodeOnStack) || []) {
    if (!e.module) continue;
    if (counts[e.module] == null) { counts[e.module] = 0; order.push(e.module); }
    counts[e.module] += 1;
  }
  for (const m of order) {
    out.stackModules.push({ name: m, hits: counts[m], kind: classifyStackKind(m) });
  }
}

function buildScriptedFrames(out) {
  out.scriptedFrames = [];
  if (out.redScriptStack && out.redScriptStack.length > 0) {
    for (let i = out.redScriptStack.length - 1; i >= 0; --i) out.scriptedFrames.push(out.redScriptStack[i]);
    return;
  }
  for (const e of (out.lastActive && out.lastActive.scriptedThreads) || []) {
    if (e.isCrashing && e.name) out.scriptedFrames.push(e.name);
  }
}

function buildGameStateLive(out) {
  if (out.lastActive && out.lastActive.liveGameState) {
    const g = out.lastActive.liveGameState;
    let any = false;
    for (const _ in g) { any = true; break; }
    if (any) out.gameStateLive = g;
  }
}

function inferFaultSiteUnreliable(out) {
  out.fingerprint = out.fingerprint || {};
  out.fingerprint.faultSiteUnreliable =
    !!(out.likelyCulprit && out.likelyCulprit.indexOf('does not lie inside any loaded module') !== -1);
}

function parseCrash(text) {
  const out = { schema: 3, source: 'crash-text-fallback', warnings: [] };
  if (!text) { out.warnings.push('empty input'); return out; }
  const lines = splitLines(text);
  const { sections, order } = splitSections(lines);
  parseHeader(sections.header || [], out);
  if (sections['Crash']) parseCrashSection(sections['Crash'], out);
  for (const label of order) {
    if (startsWith(label, 'LAST ACTIVE')) parseLastActive(sections[label], out);
    else if (startsWith(label, 'Engine state')) parseEngineStateInline(sections[label], out);
    else if (startsWith(label, 'Native call stack')) parseNativeCallstack(sections[label], out);
    else if (startsWith(label, 'RedScript call stack')) parseRedScriptCallStack(sections[label], out);
    else if (startsWith(label, 'Installed mods')) parseInstalledModsSummary(sections[label], out);
  }
  buildStackModules(out);
  buildScriptedFrames(out);
  buildGameStateLive(out);
  inferFaultSiteUnreliable(out);
  if (out.crashId == null) out.warnings.push('no crashId extracted');
  if (out.exception == null) out.warnings.push('no exception extracted');
  return out;
}
