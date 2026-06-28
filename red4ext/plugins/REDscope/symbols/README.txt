REDscope symbol folder
======================

Drop debug symbol files (.pdb) here to get named function stack frames for
modules that publish them, instead of "module.dll +0xOFFSET".

REDscope adds this folder to the Windows symbol search path at startup, so any
.pdb whose module is loaded in the game gets picked up automatically at crash
time. No setting to flip.

Symbols worth grabbing (match your installed version). On each releases page,
download the symbols/pdb zip and extract its .pdb files into this folder:
  RED4ext       "red4ext-symbols-<version>.zip"        github.com/WopsS/RED4ext/releases
  CET           "windows-latest-x64-release-pdb.zip"   github.com/maximegmd/CyberEngineTweaks/releases
  Mod Settings  "mod_settings_<version>_pdb.zip"       github.com/jackhumbert/mod_settings/releases
  Audioware     "Audioware-windows-<version>-PDB.zip"  github.com/cyb3rpsych0s1s/audioware/releases
  Input Loader  "input_loader_<version>_pdb.zip"       github.com/jackhumbert/cyberpunk2077-input-loader/releases

Most other plugins (ArchiveXL, TweakXL, Codeware, RedHotTools, RedSocket) do not
publish symbols, so their frames stay as offsets.

Two things to know:
  - The symbols must match the exact version of the DLL you have installed.
    Windows silently ignores a mismatched .pdb, so grab the pack for your
    version, not just the newest.
  - The game's own Cyberpunk2077.exe frames can't be named - CD Projekt Red
    publishes no symbols for the executable, so those stay as offsets.
