import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { analyze, validateRule } from '../core/analyzer.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');
const loadJson = (p) => JSON.parse(readFileSync(p, 'utf8'));

const engineRules = loadJson(join(root, 'rules', 'engine-signatures.json'));
const modRules = loadJson(join(root, 'rules', 'mod-attribution.json'));
const testRules = loadJson(join(here, 'test-rules.json'));
const ruleSets = [engineRules, modRules, testRules];

let failures = 0;

for (const [label, set] of [['engine', engineRules], ['mod', modRules], ['test', testRules]]) {
  for (const r of set) {
    const errs = validateRule(r);
    if (errs.length) { failures++; console.error(`INVALID ${label} rule: ${errs.join('; ')}`); }
  }
}

const orderedEq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

const cases = loadJson(join(here, 'cases.json'));
for (const c of cases) {
  const r = analyze(c.sidecar, ruleSets);
  const gotIds = r.matches.map((m) => m.id);
  const gotTop = r.topMatch ? r.topMatch.id : null;
  const gotLeads = r.leads.map((l) => l.module);
  const e = c.expected;
  const errs = [];
  if (!orderedEq(gotIds, e.matchIds)) errs.push(`matchIds: got [${gotIds}] want [${e.matchIds}]`);
  if (gotTop !== e.topMatchId) errs.push(`topMatch: got ${gotTop} want ${e.topMatchId}`);
  if (!orderedEq(gotLeads, e.leadModules || [])) errs.push(`leadModules: got [${gotLeads}] want exactly [${e.leadModules}]`);
  if (errs.length) { failures++; console.error(`FAIL ${c.name}\n  ${errs.join('\n  ')}`); }
  else console.log(`PASS ${c.name}`);
}

if (failures) { console.error(`\n${failures} failure(s)`); process.exit(1); }
console.log('\nAll conformance cases passed.');
