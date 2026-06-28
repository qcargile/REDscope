# REDscope resource-path dictionary

Offline FNV1a64-hash -> game-file-path dictionary. Resolves the `resourceLoader`
(failed / in-flight) hashes REDscope captures at crash time into readable paths,
without WolvenKit attached. Shipped as `redscope-paths.rsdb` in the CET mod's
`dictionary/` folder; read in-game by `rsdbReader.lua` and on the web by
`rsdb-reader.mjs`.

## Files

| File | Role | Committed |
|---|---|---|
| `fnv1a64.mjs` | `ResourcePath::HashSanitized` hasher (verified vs WolvenKit `compute_hash`) | yes |
| `build-dictionary.mjs` | path list -> sorted `.rsdb` binary | yes |
| `rsdb-reader.mjs` | node/web binary-search reader | yes |
| `rsdbReader.lua` (in `bin/.../REDscope/dictionary/`) | in-game binary-search reader | yes |
| `redscope-paths.rsdb` | the built dictionary (~152 MB, `all` scope) | **no** (gitignored; ships in the release zip) |

## Build pipeline (regenerate per game patch)

1. Download WolvenKit's embedded path list (KARK / Oodle-Kraken compressed, path-only):
   ```
   curl -L -o usedhashes.kark "https://raw.githubusercontent.com/WolvenKit/WolvenKit/<tag>/WolvenKit.Common/Resources/usedhashes.kark"
   ```
   Pin `<tag>` to the WolvenKit release matching the shipped game version (e.g. a 2.x tag), not `main`.
2. Decompress (Oodle Kraken). Via the WolvenKit MCP `oodle_decompress`, or WolvenKit.Core `Oodle.Decompress`, or any oo2core decompressor. Output = newline-delimited UTF-8 paths (~1.72M lines, ~129 MB).
3. Build the `.rsdb`:
   ```
   node build-dictionary.mjs --input usedhashes.txt --output redscope-paths.rsdb --mode all
   ```
   `--mode`: `all` (1.72M), `no-audio` (drop `.wem`, ~493k), `streaming` (visual/streamable types, ~390k).
4. Place `redscope-paths.rsdb` in `bin/x64/plugins/cyber_engine_tweaks/mods/REDscope/dictionary/` and include it in the Nexus release package.

## `.rsdb` format

```
0x00  "RSD1"                     4-byte magic
0x04  uint32 LE  count
0x08  uint32 LE  indexOffset (=16)
0x0C  uint32 LE  pathsOffset (=16 + count*16)
0x10  index: count x 16 bytes, sorted ascending by hash:
        [0..8)  uint64 BE  hash (FNV1a64-sanitized)
        [8..12) uint32 LE  path offset (relative to pathsOffset)
        [12..16) uint32 LE path byte length
pathsOffset  paths blob: concatenated UTF-8 paths
```

Lookup = binary-search the index by the 8-byte big-endian hash (byte-lexicographic
compare == unsigned-numeric order), then read the path from the blob. No 64-bit
arithmetic needed on the reader side.

## Validation

- Hasher matches WolvenKit `compute_hash` on canonical paths and reproduces the live
  `resolve_hash` key (9/9 round-trip across `.ent/.mesh/.app/.scene/.streamingsector/.xbm/.wem/.envprobe/.cookedanims`).
- `compute_hash` is RAW FNV1a64 (no sanitization); the hasher applies `HashSanitized`
  (lowercase + slash-normalize + collapse). They agree on canonical paths (what the
  archives store); the hasher is the principled tool for non-canonical input.
- `.rsdb` build is collision-free at all scopes (0 / 1.72M). Reader tested (`test-reader.mjs`)
  and the Lua algorithm cross-validated against the sidecar hex format (`test-lua-algorithm.mjs`).

## Licensing

`usedhashes.kark` ships in WolvenKit (GPL-3.0). The bundled artifact here is a
hash->path map of CDPR game-file paths (factual file names; an exhaustive list, not a
creative selection). Credit WolvenKit and the redmodding community as the path-list
source in the release notes. The same path facts can be regenerated independently by
enumerating the installed game archives if stricter separation is ever wanted.
