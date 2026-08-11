# REDscope

A crash logger for Cyberpunk 2077. When the game crashes, REDscope writes its
highest-ranked lead to a plain-text `.crash` file (plus a `.crash.json`
sidecar) with the call stacks, engine state, what the game was loading, recent
log errors, and which mods changed since your last working launch.

CDPR's built-in reporter collects a minidump. REDscope adds evidence you can
read and paste into a bug thread.

**[How to read a REDscope crash report &rarr;](https://qcargile.github.io/REDscope/)**
&mdash; which clues matter, what they do not prove, and what to test next.

## What you get

- A `LIKELY CULPRIT` line with a confidence rating and a lead: a specific mod
  DLL, the game's own code, a driver, or an out-of-memory condition.
- A `CRASH ID` fingerprint, so you can tell whether a fix worked and quote it in
  a report. The in-game panel also flags when the same crash is recurring.
- The native call stack, every frame attributed to its module, plus the in-flight
  RedScript call stack on the crashing thread.
- Live engine state at the crash: lifecycle stage, GPU/VRAM use, and an
  out-of-memory heuristic that separates an environmental crash from a broken mod.
- What the engine was streaming when it died, resolved to file paths through a
  path dictionary that ships with the release.
- A diff of which mods were added, removed, or updated since your last launch.
- Registers, stack memory, loaded modules, an installed-mod summary, and a
  `@wrapMethod` wrap-chain scan for the mod author.

The full per-mod modlist goes in the `.crash.json` sidecar next to the report.
Attach both when you report a crash.

## Reading a crash in-game

REDscope ships a Cyber Engine Tweaks panel that reads the latest crash without
leaving the game: the lead, the stack-module list, recent crashes, and whether
the same crash keeps coming back. There is nothing to configure.

## Install

Requires [RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380),
[Cyber Engine Tweaks](https://www.nexusmods.com/cyberpunk2077/mods/107), and
[redscript](https://www.nexusmods.com/cyberpunk2077/mods/1511). Works with MO2,
Vortex, and native installs.

Install it through your mod manager like any other mod, or unzip the folders into
your Cyberpunk 2077 directory. It should load ahead of any other crash-handler
plugin.

Crash files land in `red4ext/plugins/REDscope/crashes/`. Under MO2 that is your
Overwrite folder.

## Configuration (`REDscope.ini`)

The defaults are fine. The knobs people ask about:

- `[stack] dump_slot_count=512` &mdash; how much of the stack to emit. Lower trims
  report size; higher catches longer call chains.
- `[debug] log_level=2` &mdash; `0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=TRACE`.
- `[behavior] respect_debugger=true` &mdash; skip the handler when a debugger is
  attached, so it catches the crash first.
- `[test] inject_crash_after_seconds=0` &mdash; set to 8 to smoke-test the full
  pipeline on next launch, then reset to 0.

## What REDscope is not

- Not a GUI for the report itself. The output is plain text you paste into bug
  threads; the in-game panel is a reader, not an editor.
- Not a symbol server. Third-party DLLs without shipped PDBs are attributed by
  name and offset, and most `Cyberpunk2077.exe` frames show as raw offsets
  because only about 84 engine functions are named in the public address
  database. That is normal.
- Not a replacement for a debugger. Attach to `Cyberpunk2077.exe` for live
  debugging; REDscope detects a debugger and defers.

## For mod authors

REDscope keeps a ring of breadcrumbs and prints the last entries next to the call
stacks. You can add your own from RedScript or CET for the moments that matter.
See the
[mod-author section of the guide](https://qcargile.github.io/REDscope/#mod-authors).

## License

MIT. See [LICENSE](LICENSE).

## Bugs / feature requests

[github.com/qcargile/REDscope](https://github.com/qcargile/REDscope). Attach the
`.crash` and `.crash.json` from `red4ext/plugins/REDscope/crashes/`; the internal
log (`REDscope-*.log`, same dir) records what each step attempted.
