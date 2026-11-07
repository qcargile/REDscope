# REDscope conflict database

Drop `*.json` files in this folder to tell REDscope which mod combinations are known to conflict. REDscope loads every `.json` here (except names starting with `_`) on startup, cross-references them against your installed mods, and surfaces any active conflict in the crash report and the diagnostic panel as a lead.

A flagged conflict is a lead, not a verdict. Intentional base-plus-patch pairs and benign overlaps can show up here, so verify in-game before changing your load order.

"Installed" means a mod is enabled in your mod manager and ships the named archive on disk. REDscope does not resolve final load order, so a flagged archive may end up shadowed or unused at runtime.

## File format

```json
{
  "mods": {
    "Display Name":        "SomeArchive.archive",
    "Other Mod":           ["First.archive", "Second.archive", "sometag"]
  },
  "dependencies": {
    "Display Name": ["Required Mod", "Phantom Liberty"]
  },
  "conflicts": [
    ["Mod A", "Mod B"]
  ],
  "exceptions": [
    ["Mod A", "Mod B"]
  ]
}
```

- `mods` maps a display name to the `.archive` file(s) that mod ships. A value that is an array may also contain non-`.archive` strings, which are treated as tags.
- Two mods that share a tag are treated as conflicting (only one of each tag should be installed). Use this for categories like environment, LUT, or rain mods.
- `dependencies` lists mods a given mod needs. The literal `Phantom Liberty` checks for the DLC instead of a mod.
- `conflicts` lists explicit groups that fight even without a shared tag.
- `exceptions` lists groups that are allowed to coexist, overriding a tag or explicit conflict.

When the same display name appears in more than one file, the first definition wins.

## Credit

The file format and the detection logic mirror CyanideX's Conflict Begone (Nexus 21912). REDscope re-implements the matching engine; it does not ship that mod's database. Add your own conflict files here, or drop in a community-maintained one that uses this format.
