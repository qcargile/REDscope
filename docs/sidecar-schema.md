# `.crash.json` sidecar schema

Current schema: **5**.

The sidecar is the analyzer's primary input format. Every field below is either consumed by an analyzer rule condition OR marked `telemetry-only`. New fields should land with at least one rule that exercises them (or an annotation that they are deliberately not analyzer input).

## Analyzer-input fields (rule conditions consume these)

| Field | Used by condition(s) |
|---|---|
| `schema` | `minSchema` |
| `crashClass` | `crashClassAny` |
| `exception.code` | `exceptionCodeAny` |
| `faultingModule.name` | `faultingModuleAny` |
| `engineState.state` | `engineStateAny` |
| `engineState.oomSuspected` | `oomSuspected` |
| `engineState.oomBasis` | `oomBasisContains` (added schema 3) |
| `scriptedFrames[]` | `scriptedFrameAny`/`All`/`None` |
| `stackModules[]` | `stackModuleAny`/`All`/`None` |
| `installedMods[]` | `installedModAny` |
| `installedMods[]` (name + version) | `installedModVersionMatches` (added schema 3) |
| `tweakDbLookups[].recordName` | `tweakDbRecordAny` (added schema 3) |
| `statusEffectChanges[].recordName` | `statusEffectRecordAny` (added schema 3) |
| `findEntityNulls[].callerTag` | `findEntityNullCallerAny` (added schema 3, substring) |
| `recentDelays[].callbackClass` | `recentDelayCallbackAny` (added schema 3, substring) |
| `wrapChains[].layers[].modName` | `wrapLayerModAny` (added schema 3) |
| `objectsInFlight[].className` | `inFlightClassAny` (added schema 4) |
| `objectsInFlight[].modFields` | `inFlightClassHasModFields` (added schema 4) |
| `archiveConflicts.curated.active[]` | `curatedConflictActive` (bool, added schema 5) |
| `archiveConflicts.curated.active[].mods[]` | `conflictGroupAny` (added schema 5) |
| `setupIntegrity.issues[]` | `setupHasIssues` (bool, added schema 5) |
| `setupIntegrity.issues[].kind` | `setupIssueKindAny` (added schema 5) |

`objectsInFlight` (schema 4) is the RTTI class of every heap object REDscope could identify in the crashing thread's CPU registers at fault time, each with the count of mod-added scripted fields on that class (`modFields`). It surfaces *what* the engine was operating on when the fault PC is vanilla code — the data axis that the module/stack axes miss. `modFields > 0` is an attribution lead (some installed mod `@addField`s that class), never a verdict. Reversing a record/resource *name* from a faulting `TweakDBID`/`ResourcePath` is not possible at runtime (both are hashes with no engine-side reverse pool), so attribution here is by class identity + mod-field presence, not record ownership.

`setupIntegrity` (schema 5) is the result of the install-health pass: `{ issues: [{kind, detail}], truncated }`. `kind` is a stable machine tag (`missing-framework`, `plugin-load-failure`); `detail` is the human sentence. These are measured facts about the install (a framework a mod needs is not loaded, or RED4ext reported a plugin load failure), computed on the worker thread from the inventory + loaded-module list + the red4ext log — not from any crash. `missing-framework` is high-confidence (the mod genuinely does nothing without its framework). The pass deliberately omits version-floor / stale-framework detection, which would require a per-game-build floors table.

`archiveConflicts` (schema 5) is the result of the curated conflict engine: `{ curated: { active: [{mods:[...]}], missingDeps: [{mod, requires:[...]}], dbModCount, truncated } }`. `active` groups are sets of installed mods the curated DB flags as mutually conflicting (tag-implicit or explicit, with exception pairs removed); `missingDeps` are installed mods whose declared dependencies (including the `Phantom Liberty` DLC sentinel) are unmet. Computed on the worker thread from the installed-archive set + any `conflicts/*.json` dropped in the plugin dir — the shipped seed is REDscope-authored, not the third-party DB. A `curatedConflictActive` match is a **lead, not a verdict** (intentional base+patch pairs can appear); the rule that consumes it is medium-confidence by design.

## Telemetry-only fields (not for rule matching — retained for paste-tool / debugging)

These fields are carried for human + paste-tool readers. They are intentionally NOT consumed by analyzer rules. Removing them is schema-breaking; do not remove without locking down the consumer surface first.

- `fingerprint.{primary,loose,modSet}` — 16-hex-char raw hashes. Used internally for grouping; rule matching uses the higher-level `crashId`/`looseId`.
- `stackModulesScanned`, `stackModulesOverflow` — counters from the stack-memory scan. Useful in reports for "how confident is the stack walk"; never a rule input.
- `callStackMods[].topFrame` — frame index where the mod DLL first appeared on the unwound stack. UI shows "frame 47" labels; not a rule input.

## Render-only fields (UI consumes; analyzer does not)

- `gameStateLive` — `{ key: value }` map of live Lua-pushed state (Item 5). Panel + web tool render it; no rule key (we expect users to inspect it themselves, not write rules against it).
- `modsChangedSinceLastLaunch.{added,removed,updated}` — surface as the hero block in both UIs. No rule key today; a future rule could match on `added[].name` if recurrence shows it's worth a condition.
- `gameUptimeNs` — total time-into-session in ns (added schema 3). Currently UI-only; a future `gameUptimeNsMax`/`Min` condition would let rules separate "always at load" from "always after an hour."

## Schema-version compatibility

Rules MAY gate on `minSchema` to opt into newer fields. Older sidecars (schema < 3) lack the new arrays and the rule sees them as absent (no match). New sidecars (schema 3+) are backward compatible with older rules — additive fields, no renames, no removals.
