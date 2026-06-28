import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const core = join(here, '..', 'core');
const luaSrc = readFileSync(join(core, 'analyzer.lua'), 'utf8');
const jsSrc = readFileSync(join(core, 'analyzer.js'), 'utf8');

let failures = 0;
function fail(msg) { failures++; console.error('TWIN PARITY FAIL: ' + msg); }

function luaTableKeys(src, name) {
  const re = new RegExp('local\\s+' + name + '\\s*=\\s*\\{([\\s\\S]*?)\\}');
  const m = src.match(re);
  if (!m) return null;
  const keys = new Set();
  const kre = /([A-Za-z_][A-Za-z0-9_]*)\s*=\s*true/g;
  let k;
  while ((k = kre.exec(m[1]))) keys.add(k[1]);
  return keys;
}

function jsSetItems(src, name) {
  const re = new RegExp('const\\s+' + name + '\\s*=\\s*new Set\\(\\[([\\s\\S]*?)\\]\\)');
  const m = src.match(re);
  if (!m) return null;
  const keys = new Set();
  const kre = /'([^']+)'/g;
  let k;
  while ((k = kre.exec(m[1]))) keys.add(k[1]);
  return keys;
}

function slice(src, startMarker, endMarker) {
  const s = src.indexOf(startMarker);
  if (s < 0) return '';
  const e = src.indexOf(endMarker, s + startMarker.length);
  return src.slice(s, e > s ? e : src.length);
}

function matchRuleConds(body) {
  const keys = new Set();
  const re = /\bm\.([A-Za-z_][A-Za-z0-9_]*)/g;
  let k;
  while ((k = re.exec(body))) keys.add(k[1]);
  keys.delete('match');
  return keys;
}

function eqSets(a, b) {
  if (a.size !== b.size) return false;
  for (const x of a) if (!b.has(x)) return false;
  return true;
}

function diff(a, b) {
  return {
    onlyLua: [...a].filter((x) => !b.has(x)),
    onlyJs: [...b].filter((x) => !a.has(x)),
  };
}

for (const name of ['VALID_CONDITIONS', 'VALID_VERDICTS', 'VALID_CONFIDENCE']) {
  const lua = luaTableKeys(luaSrc, name);
  const js = jsSetItems(jsSrc, name);
  if (!lua) { fail(name + ' not found in analyzer.lua'); continue; }
  if (!js) { fail(name + ' not found in analyzer.js'); continue; }
  if (!eqSets(lua, js)) {
    const d = diff(lua, js);
    fail(name + ' diverges: only-in-lua=[' + d.onlyLua + '] only-in-js=[' + d.onlyJs + ']');
  } else {
    console.log('OK ' + name + ' (' + lua.size + ' entries match)');
  }
}

const luaBody = slice(luaSrc, 'function Analyzer.matchRule', 'local function matchLess');
const jsBody = slice(jsSrc, 'export function matchRule', 'function compareMatched');
const luaHandled = matchRuleConds(luaBody);
const jsHandled = matchRuleConds(jsBody);

if (!eqSets(luaHandled, jsHandled)) {
  const d = diff(luaHandled, jsHandled);
  fail('matchRule handles different conditions: only-in-lua=[' + d.onlyLua + '] only-in-js=[' + d.onlyJs + ']');
} else {
  console.log('OK matchRule handles the same ' + luaHandled.size + ' conditions in both twins');
}

const validLua = luaTableKeys(luaSrc, 'VALID_CONDITIONS') || new Set();
for (const cond of validLua) {
  if (!luaHandled.has(cond)) fail('condition "' + cond + '" is in VALID_CONDITIONS but unhandled in analyzer.lua matchRule');
  if (!jsHandled.has(cond)) fail('condition "' + cond + '" is in VALID_CONDITIONS but unhandled in analyzer.js matchRule');
}
for (const cond of luaHandled) {
  if (!validLua.has(cond)) fail('condition "' + cond + '" is handled in matchRule but missing from VALID_CONDITIONS');
}

if (failures) { console.error('\n' + failures + ' twin-parity failure(s)'); process.exit(1); }
console.log('\nAll twin-parity checks passed.');
