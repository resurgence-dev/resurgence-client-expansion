# rcx module

The C++ side of the `resurgence-client-expansion` addon: `gmcl_rcx_win64.dll`, an optional client
binary module (VR, the arm IK). Its Lua API, input files and session rules are in the addon's
`README.md`; the design is in `gamemodes/Resurgence/docs/client-expansion-design.md` and
`docs/vr-design.md`. Binary modules can't ship over the Workshop.

## Building and installing (Windows)

One command, from this folder:

```bash
build_module.bat -Install
```

`build_module.bat` (double-click, or run it with arguments) runs `tools/build_module.ps1`, which
builds the module with MSVC through the CMake that Visual Studio bundles, as
`resurgence-nav/module/build_modules.bat` does for RNav, into `builds/` (gitignored):

- `builds/win64/`: `gmcl_rcx_win64.dll`, `openvr_api.dll` beside it, the test exes;
- `builds/cmake-win/`: the CMake tree.

Then it runs the module's own tests, and a failure fails the build: `rcx_ik_test.exe` (the IK
solver), and `tests/rcx_input_files_test.exe` with `tools/check_input_files.py` (the VR action
manifest and default bindings written without SteamVR, then parsed and checked; Python 3 on the
path). It prints the version it built (from `kVersion`), the files with their sizes and times, and
`OK` or `FAILED` (exit code 1).

Without `-Install` it never writes `garrysmod/lua/bin`, so it works while a game has the module
loaded. With it:

1. **The lock check.** It refuses, with the game's pid, while any `gmod.exe` (or `hl2.exe`,
   `srcds.exe`) has `lua/bin/gmcl_rcx_win64.dll` loaded, or the file is locked. GMod keeps a loaded
   module until the process exits, and the module pins itself once VR starts. Close that game
   first; a session never closes the maintainer's game. The harness's persistent client closes
   after its run with `linux-test/lt.bat client stop`.
2. **The rollback copy.** The installed `gmcl_rcx_win64.dll` and `openvr_api.dll` are copied into
   `lua/bin/rcx_previous/` first (GMod doesn't look in subfolders, so it never loads them).
3. **The install.** Both new files are copied into `lua/bin`.

`build_module.bat -Rollback` puts `rcx_previous/` back into `lua/bin`, with the same lock check
and no build. Other arguments: `-Clean` (delete the CMake tree first), `-Config Debug`.

A session runs the script directly, since the bat ends on `pause`:
`powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_module.ps1 -Install`.

Requirements and the two `third_party` clones are in the addon's `README.md`, Building.

## Linux

Not built yet: the module is client-only and Windows-only for now.

## Version

`kVersion` in `src/module.cpp`, read in game with `rcx.Version()` (`rsrg_vr_status` prints it).
Bump it on every build that changes behavior (`0.54` to `0.55`), naming the change after the
number; a rebuild with no code change keeps it. Lua checks the number (`ModuleRuntime.VersionNumber`
in `gamemode/plugins/vr/cl_runtime.lua`) or the function it needs, and falls back with one
warning when the module is older. There is no version file: RNav's `gmsv_rnav.ver` is its Linux
export list, not a version.
