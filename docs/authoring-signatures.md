# Authoring crash signatures

A signature is one JSON rule that matches a crash and emits a verdict. The analyzer (the Lua panel and the web JS twin) loads every rule, tests each against the crash sidecar, and the highest-ranked match becomes the culprit line.

## Where they live

- `analyzer/rules/engine-signatures.json` — engine and environment level (OOM, GPU resets, init-time, hangs). Already seeded.
- `analyzer/rules/mod-attribution.json` — mod-specific signatures. This is the one that grows. Starts as `[]`.

Both files are arrays of rule objects. Add a signature by appending an object to the array.

## Rule shape

```json
{
  "id": "MOD-FOO-TOOLTIP-1.2.0",
  "name": "Foo 1.2.0 weapon-tooltip crash",
  "severity": 5,
  "verdict": "mod",
  "confidence": "high",
  "match": { },
  "message": "What it is, in plain language.",
  "fix": "What to do about it.",
  "links": [{ "label": "Foo on Nexus", "url": "https://www.nexusmods.com/cyberpunk2077/mods/0000" }]
}
```

- `id` — unique string. Convention: `MOD-<NAME>-<WHAT>`, version-suffixed when the signature is version-specific.
- `severity` — integer 1 to 6. Higher sorts first.
- `verdict` — one of `mod`, `engine`, `memory`, `environmental`, `unknown`.
- `confidence` — one of `high`, `medium`, `low`. Breaks severity ties.
- `message` / `fix` — plain text shown to the user.
- `links` — array, optional. Each entry is `{label, url}` or a bare URL string. Use `[]` for none.

## Match conditions

A `match` is a set of conditions, and all of them must pass (AND). One weak condition fires too often; pin a signature down with two or three.

| Condition | Tests against | Example |
|---|---|---|
| `exceptionCodeAny` | exception code | `["0xC0000005"]` |
| `crashClassAny` | crash class | `["scripted"]`, `["Hang"]` |
| `faultingModuleAny` | faulting module name | `["icuuc.dll"]` |
| `stackModuleAny` / `All` / `None` | DLLs in the native stack | `["ArchiveXL.dll"]` |
| `scriptedFrameAny` / `All` / `None` | scripted frame names (substring) | `["FetchModsDataPackages"]` |
| `installedModAny` | an installed mod by name | `["Foo"]` |
| `installedModVersionMatches` | a mod at an exact version | `[{"name":"Foo","version":"1.2.0"}]` |
| `wrapLayerModAny` | a mod whose @wrapMethod is in the chain | `["Foo"]` |
| `tweakDbRecordAny` | a TweakDB record looked up near the crash | `["Items.Foo"]` |
| `statusEffectRecordAny` | a status effect changed near the crash | `["BaseStatusEffect.Foo"]` |
| `findEntityNullCallerAny` | a FindEntityByID null caller (substring) | `["Foo"]` |
| `recentDelayCallbackAny` | a recent delay callback class (substring) | `["FooDelay"]` |
| `engineStateAny` | engine lifecycle state | `["Initialization"]` |
| `oomSuspected` | the OOM heuristic | `true` |
| `oomBasisContains` | OOM basis (substring) | `["vram"]`, `["commit"]` |
| `minSchema` | require sidecar schema >= N | `3` |

`*Any` passes when at least one entry matches, `*All` when all match, `*None` when none are present. The four marked substring match partial names; the rest match exactly. All matching is case-insensitive.

## Ranking

When several rules match, they sort by severity (descending), then confidence (high > medium > low), then file order. The top one is the headline verdict; the rest still show beneath it.

## Worked example

A user reports that with Foo 1.2.0, hovering a weapon crashes. Their `.crash.json` shows `faultingModule: icuuc.dll`, `crashClass: scripted`, a scripted frame ending in `FetchModsDataPackages`, and Foo 1.2.0 installed.

```json
{
  "id": "MOD-FOO-TOOLTIP-1.2.0",
  "name": "Foo 1.2.0 weapon-tooltip crash",
  "severity": 5,
  "verdict": "mod",
  "confidence": "high",
  "match": {
    "faultingModuleAny": ["icuuc.dll"],
    "crashClassAny": ["scripted"],
    "scriptedFrameAny": ["FetchModsDataPackages"],
    "installedModVersionMatches": [{ "name": "Foo", "version": "1.2.0" }]
  },
  "message": "Foo 1.2.0 adds float fields to weapon records; the tooltip formatter overruns on an affected weapon.",
  "fix": "Update Foo to 1.2.1, which drops the bad fields.",
  "links": [{ "label": "Foo on Nexus", "url": "https://www.nexusmods.com/cyberpunk2077/mods/0000" }]
}
```

The version pin means the signature stops matching once the user updates Foo, so it never hands out a stale verdict.

## Shipping a new signature

1. Append the rule to `analyzer/rules/mod-attribution.json` and keep the file valid JSON.
2. Sync to the modlist so the in-game panel picks it up.
3. Rebuild the web tool: `node analyzer/web/build-web.mjs`, then publish `docs/index.html` to the public repo.
4. Cut a REDscope update so installed users get the new panel rules.

## Validate before shipping

`validateRule` in `analyzer/core/analyzer.lua` checks the id, the severity range, the verdict and confidence enums, and that every match key is real. The conformance suite under `analyzer/conformance/` runs the Lua and JS analyzers against the same cases so the two never drift; run the `AnalyzerConformance` CTest target after adding rules.
