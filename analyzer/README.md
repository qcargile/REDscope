# REDscope analyzer

The crash analyzer foundation: a declarative rule engine, shared by the in-game CET panel
(`core/analyzer.lua`) and the client-side web paste-tool (`core/analyzer.js`). Both interpret
the same JSON rule data and the same `.crash.json` sidecar that REDscope writes at crash time.

> **Lead, not verdict.** This tool runs locally on one user's crashes with no shared telemetry
> corpus. It surfaces *leads* — the crash's own evidence (faulting module, modules present in the
> crash stack, a matched rule). It never claims a confident culprit. Statistical "lift over a
> population baseline" is impossible at N=1 and is deliberately not attempted.

## Layout

```
analyzer/
  core/analyzer.js        reference matcher (JS) — consumed by the web tool, tested by conformance
  core/analyzer.lua       mirror matcher (Lua)   — consumed by the CET panel
  rules/engine-signatures.json   stable: engine/vanilla/environmental crash signatures
  rules/mod-attribution.json     churning: per-mod known-bad signatures (bootstraps empty)
  schema/rules.schema.json       JSON Schema for a rule set (editor tooling + documentation)
  conformance/cases.json         shared spec: sidecar -> expected verdict
  conformance/run.mjs            node runner (also run by CTest as AnalyzerConformance)
```

## The rule format

A rule set is a JSON array of rule objects. A rule **matches** a sidecar when **every** condition
present in its `match` object is satisfied (conditions AND together; an absent condition is ignored).

```json
{
  "id": "ENG-GPU-TDR",
  "name": "GPU device removed/reset (TDR)",
  "severity": 4,
  "verdict": "environmental",
  "confidence": "medium",
  "match": { "exceptionCodeAny": ["0x887A0005", "0x887A0006", "0x887A0007"] },
  "message": "The GPU was removed or reset ... not a mod.",
  "fix": "Clean-install GPU drivers; revert overclocks.",
  "links": []
}
```

- `severity` is an integer 1-6 (higher sorts first). `verdict` is one of
  `environmental | mod | engine | memory | unknown`. `confidence` is `high | medium | low`.

### Match conditions

| Condition | Satisfied when |
|---|---|
| `minSchema` | `sidecar.schema >= value` (gate rules that need fields a v1 sidecar lacks, e.g. `stackModules`) |
| `exceptionCodeAny` | `exception.code` equals one listed code (e.g. `"0xC0000005"`, case-insensitive) |
| `crashClassAny` | `crashClass` equals one listed value |
| `faultingModuleAny` | `faultingModule.name` equals one listed module |
| `stackModuleAll` / `stackModuleAny` / `stackModuleNone` | all / any / none of the listed modules appear in `stackModules[].name` |
| `scriptedFrameAll` / `scriptedFrameAny` / `scriptedFrameNone` | all / any / none of the listed substrings appear in any `scriptedFrames[]` entry |
| `installedModAny` | one listed name appears in `installedMods[].name` |
| `oomSuspected` | `engineState.oomSuspected` equals the boolean value |
| `engineStateAny` | `engineState.state` equals one listed value |

`*Any` = OR within the list, `*All` = AND, `*None` = NOR. Module and code comparisons are
case-insensitive **exact** matches; scripted-frame matching is **substring**. Module-name
conditions (`faultingModuleAny`, `stackModule*`) match the loaded-DLL basename, so list the
**full filename including `.dll`** (e.g. `"ArchiveXL.dll"`, not `"ArchiveXL"`). `installedModAny`
matches the mod's installed name (no extension) — a different namespace from the DLL basename.

### Result shape

`analyze(sidecar, ruleSets)` returns `{ schema, crashId, facts, matches, topMatch, leads }`.
`matches` is every matched rule, sorted by `severity` desc, then `confidence` desc, then load
order (fully deterministic, so the Lua and JS matchers produce identical order). `topMatch` is the
first match or `null`. `leads` is the evidence-weighted lead list — mod-kind modules present in the
crash stack — populated even when no rule matches (the facts-only floor). **A lead is never a
confident culprit**; the field is named `topMatch`, not `topVerdict`, on purpose.

### Verdict and confidence

`verdict` is a coarse responsibility bucket (`environmental | mod | engine | memory | unknown`),
not the same taxonomy as the C++ crash report's `LIKELY CULPRIT` category (which carries a distinct
`stack-overflow` category, collapsed to `engine` here). The engine-rule `confidence` values mirror
`src/report/CulpritHeuristic.cpp` (TDR and OOM are `low`; heap / stack-buffer-overrun / stack-overflow
are `high`; in-page is `medium`) so the analyzer and the C++ report agree.

## Two tiers

- **`engine-signatures.json`** — vanilla / engine / environmental crashes (GPU TDR, OOM, stack
  overflow, heap corruption). Stable; keyed off exception codes and engine state, so it is
  build-independent (no `minSchema`).
- **`mod-attribution.json`** — per-mod known-bad signatures. This is the living asset and it
  ships **empty**: Skyrim/Fallout rule databases do not port. It grows from real REDscope reports.
  Rules that match on `stackModules` must set `"minSchema": 2`.

## Adding a rule

1. Add an object to the appropriate tier file.
2. Run `node conformance/run.mjs` (or `npm test`). `validateRule` rejects unknown conditions,
   bad severity/verdict/confidence, and empty `match`.
3. Add a conformance case to `conformance/cases.json` proving the rule fires on the intended
   sidecar and does not fire on a near-miss.

## Keeping the two matchers in lock-step

`analyzer.js` is the tested reference. `analyzer.lua` is a line-by-line mirror (same function
names, same control flow). `conformance/cases.json` is the shared spec: the node runner checks
the JS matcher in CI/CTest; the CET panel runs the **same** `cases.json` through the Lua matcher
in-game (authentic CET `json.decode` + Lua 5.x). Any change to one matcher must keep the
conformance corpus green on both.
