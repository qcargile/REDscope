const CONFIDENCE_RANK = { high: 3, medium: 2, low: 1 };

const FRAMEWORK_DLLS = new Set([
  'redscope.dll', 'red4ext.dll', 'cyber_engine_tweaks.asi', 'scc.dll',
]);

const VALID_CONDITIONS = new Set([
  'minSchema', 'exceptionCodeAny', 'crashClassAny', 'faultingModuleAny',
  'stackModuleAll', 'stackModuleAny', 'stackModuleNone',
  'scriptedFrameAll', 'scriptedFrameAny', 'scriptedFrameNone',
  'installedModAny', 'installedModVersionMatches',
  'oomSuspected', 'oomBasisContains', 'engineStateAny',
  'tweakDbRecordAny', 'statusEffectRecordAny',
  'findEntityNullCallerAny', 'recentDelayCallbackAny',
  'wrapLayerModAny',
  'inFlightClassAny',
  'curatedConflictActive', 'conflictGroupAny',
  'setupHasIssues', 'setupIssueKindAny',
]);
const VALID_VERDICTS = new Set(['environmental', 'mod', 'engine', 'memory', 'unknown']);
const VALID_CONFIDENCE = new Set(['high', 'medium', 'low']);

function lc(s) { return (s == null ? '' : String(s)).toLowerCase(); }
function asArray(x) { return Array.isArray(x) ? x : []; }

function stackModuleNames(sidecar) {
  return asArray(sidecar.stackModules).map((m) => lc(m && m.name));
}
function scriptedFrames(sidecar) {
  return asArray(sidecar.scriptedFrames).map((f) => lc(f));
}
function anySubstring(haystacks, needle) {
  const n = lc(needle);
  for (const h of haystacks) { if (h.indexOf(n) !== -1) return true; }
  return false;
}

export function matchRule(rule, sidecar) {
  const m = rule && rule.match;
  if (!m) return false;

  if (m.minSchema != null && !(Number(sidecar.schema) >= Number(m.minSchema))) return false;

  if (m.exceptionCodeAny) {
    const code = lc(sidecar.exception && sidecar.exception.code);
    if (!asArray(m.exceptionCodeAny).some((c) => lc(c) === code)) return false;
  }
  if (m.crashClassAny) {
    const cc = lc(sidecar.crashClass);
    if (!asArray(m.crashClassAny).some((c) => lc(c) === cc)) return false;
  }
  if (m.faultingModuleAny) {
    const fm = sidecar.faultingModule;
    const name = fm ? lc(fm.name) : null;
    if (name == null || !asArray(m.faultingModuleAny).some((x) => lc(x) === name)) return false;
  }
  if (m.stackModuleAll || m.stackModuleAny || m.stackModuleNone) {
    const mods = stackModuleNames(sidecar);
    const has = (x) => mods.indexOf(lc(x)) !== -1;
    if (m.stackModuleAll && !asArray(m.stackModuleAll).every(has)) return false;
    if (m.stackModuleAny && !asArray(m.stackModuleAny).some(has)) return false;
    if (m.stackModuleNone && asArray(m.stackModuleNone).some(has)) return false;
  }
  if (m.scriptedFrameAll || m.scriptedFrameAny || m.scriptedFrameNone) {
    const frames = scriptedFrames(sidecar);
    const has = (sub) => anySubstring(frames, sub);
    if (m.scriptedFrameAll && !asArray(m.scriptedFrameAll).every(has)) return false;
    if (m.scriptedFrameAny && !asArray(m.scriptedFrameAny).some(has)) return false;
    if (m.scriptedFrameNone && asArray(m.scriptedFrameNone).some(has)) return false;
  }
  if (m.installedModAny) {
    const names = asArray(sidecar.installedMods).map((x) => lc(x && x.name));
    if (!asArray(m.installedModAny).some((x) => names.indexOf(lc(x)) !== -1)) return false;
  }
  if (m.installedModVersionMatches) {
    const installed = asArray(sidecar.installedMods);
    const needles = asArray(m.installedModVersionMatches);
    const ok = needles.some((n) => {
      const target = lc(n && n.name);
      const ver = lc(n && n.version);
      if (!target || !ver) return false;
      return installed.some((im) => lc(im && im.name) === target && lc(im && im.version) === ver);
    });
    if (!ok) return false;
  }
  if (m.oomSuspected != null) {
    const oom = !!(sidecar.engineState && sidecar.engineState.oomSuspected);
    if (oom !== !!m.oomSuspected) return false;
  }
  if (m.oomBasisContains) {
    const basis = lc(sidecar.engineState && sidecar.engineState.oomBasis);
    if (!basis) return false;
    if (!asArray(m.oomBasisContains).some((s) => basis.indexOf(lc(s)) !== -1)) return false;
  }
  if (m.engineStateAny) {
    const st = lc(sidecar.engineState && sidecar.engineState.state);
    if (!asArray(m.engineStateAny).some((x) => lc(x) === st)) return false;
  }
  if (m.tweakDbRecordAny) {
    const lookups = asArray(sidecar.tweakDbLookups).map((e) => lc(e && e.recordName));
    if (!asArray(m.tweakDbRecordAny).some((x) => lookups.indexOf(lc(x)) !== -1)) return false;
  }
  if (m.statusEffectRecordAny) {
    const recs = asArray(sidecar.statusEffectChanges).map((e) => lc(e && e.recordName));
    if (!asArray(m.statusEffectRecordAny).some((x) => recs.indexOf(lc(x)) !== -1)) return false;
  }
  if (m.findEntityNullCallerAny) {
    const callers = asArray(sidecar.findEntityNulls).map((e) => lc(e && e.callerTag));
    const has = (sub) => callers.some((c) => c.indexOf(lc(sub)) !== -1);
    if (!asArray(m.findEntityNullCallerAny).some(has)) return false;
  }
  if (m.recentDelayCallbackAny) {
    const cbs = asArray(sidecar.recentDelays).map((e) => lc(e && e.callbackClass));
    const has = (sub) => cbs.some((c) => c.indexOf(lc(sub)) !== -1);
    if (!asArray(m.recentDelayCallbackAny).some(has)) return false;
  }
  if (m.wrapLayerModAny) {
    const mods = [];
    for (const ch of asArray(sidecar.wrapChains)) {
      for (const l of asArray(ch && ch.layers)) mods.push(lc(l && l.modName));
    }
    if (!asArray(m.wrapLayerModAny).some((x) => mods.indexOf(lc(x)) !== -1)) return false;
  }
  if (m.inFlightClassAny) {
    const classes = asArray(sidecar.objectsInFlight).map((o) => lc(o && o.className));
    if (!asArray(m.inFlightClassAny).some((x) => classes.indexOf(lc(x)) !== -1)) return false;
  }
  if (m.curatedConflictActive != null) {
    const ac = sidecar.archiveConflicts;
    const active = ac && ac.curated ? asArray(ac.curated.active) : [];
    if ((active.length > 0) !== !!m.curatedConflictActive) return false;
  }
  if (m.conflictGroupAny) {
    const ac = sidecar.archiveConflicts;
    const active = ac && ac.curated ? asArray(ac.curated.active) : [];
    const mods = [];
    for (const g of active) for (const mn of asArray(g && g.mods)) mods.push(lc(mn));
    if (!asArray(m.conflictGroupAny).some((x) => mods.indexOf(lc(x)) !== -1)) return false;
  }
  if (m.setupHasIssues != null) {
    const si = sidecar.setupIntegrity;
    const issues = si ? asArray(si.issues) : [];
    if ((issues.length > 0) !== !!m.setupHasIssues) return false;
  }
  if (m.setupIssueKindAny) {
    const si = sidecar.setupIntegrity;
    const kinds = (si ? asArray(si.issues) : []).map((iss) => lc(iss && iss.kind));
    if (!asArray(m.setupIssueKindAny).some((x) => kinds.indexOf(lc(x)) !== -1)) return false;
  }
  return true;
}

function compareMatched(a, b) {
  const s = (b.rule.severity || 0) - (a.rule.severity || 0);
  if (s) return s;
  const c = (CONFIDENCE_RANK[b.rule.confidence] || 0) - (CONFIDENCE_RANK[a.rule.confidence] || 0);
  if (c) return c;
  return a.ord - b.ord;
}

function evidenceLeads(sidecar) {
  const leads = [];
  const seen = {};
  const stackByName = {};
  for (const sm of asArray(sidecar.stackModules)) {
    if (sm && sm.name) stackByName[lc(sm.name)] = sm;
  }
  const push = (name, hits, why) => {
    const k = lc(name);
    if (!name || seen[k]) return;
    seen[k] = true;
    leads.push({ module: name, hits: hits || 0, why });
  };
  const fm = sidecar.faultingModule;
  if (fm && fm.name) {
    const sm = stackByName[lc(fm.name)];
    if (sm && lc(sm.kind) === 'mod') push(fm.name, sm.hits, 'fault inside a mod module');
  }
  for (const sm of asArray(sidecar.stackModules)) {
    if (!sm || lc(sm.kind) !== 'mod') continue;
    if (FRAMEWORK_DLLS.has(lc(sm.name))) continue;
    push(sm.name, sm.hits, 'mod code present in crash stack');
  }
  return leads;
}

export function analyze(sidecar, ruleSets) {
  const rules = [];
  for (const set of asArray(ruleSets)) for (const r of asArray(set)) rules.push(r);
  const matched = [];
  rules.forEach((rule, ord) => { if (matchRule(rule, sidecar)) matched.push({ rule, ord }); });
  const matchedIds = new Set(matched.map((x) => x.rule.id));
  const kept = matched.filter(({ rule }) => !asArray(rule.suppressedBy).some((id) => matchedIds.has(id)));
  kept.sort(compareMatched);
  const matches = kept.map(({ rule: r }) => ({
    id: r.id, name: r.name, severity: r.severity, verdict: r.verdict,
    confidence: r.confidence, message: r.message, fix: r.fix, links: r.links || [],
  }));
  return {
    schema: sidecar.schema,
    crashId: sidecar.crashId,
    facts: {
      exceptionCode: sidecar.exception && sidecar.exception.code,
      faultingModule: sidecar.faultingModule ? sidecar.faultingModule.name : null,
      crashClass: sidecar.crashClass,
    },
    matches,
    topMatch: matches.length ? matches[0] : null,
    leads: evidenceLeads(sidecar),
  };
}

export function validateRule(rule) {
  const errs = [];
  if (!rule || typeof rule !== 'object') return ['rule is not an object'];
  const id = rule.id || '(no id)';
  if (!rule.id || typeof rule.id !== 'string') errs.push('missing string id');
  if (typeof rule.severity !== 'number' || rule.severity < 1 || rule.severity > 6) {
    errs.push(`${id}: severity must be an integer 1-6`);
  }
  if (!VALID_VERDICTS.has(rule.verdict)) errs.push(`${id}: invalid verdict "${rule.verdict}"`);
  if (!VALID_CONFIDENCE.has(rule.confidence)) errs.push(`${id}: invalid confidence "${rule.confidence}"`);
  const m = rule.match;
  if (!m || typeof m !== 'object' || Array.isArray(m) || Object.keys(m).length === 0) {
    errs.push(`${id}: "match" must be an object with at least one condition`);
  } else {
    for (const k of Object.keys(m)) {
      if (!VALID_CONDITIONS.has(k)) errs.push(`${id}: unknown match condition "${k}"`);
    }
  }
  if (!rule.message || typeof rule.message !== 'string') errs.push(`${id}: missing string message`);
  return errs;
}
