# Resurgence Client Expansion

An optional client binary module for Garry's Mod, `gmcl_rcx_win64.dll`, loaded with
`require("rcx")`. It gives client Lua what Lua can't do on its own. Its first component is VR
(`rcx.vr`): an OpenVR session, head and hand poses, and the game's picture handed to the headset.
The second is a body IK solver (`rcx.ik`), pure math with no engine access.
The designs are `docs/client-expansion-design.md` and `docs/vr-design.md` in the Resurgence
gamemode; this addon is the module and a standalone testbed, and runs in any gamemode.

Windows x64 only (the 64-bit game branch). The game never needs it: Lua checks `rcx` and
`rcx.Features()` and copes with it missing.

## What loading does

Nothing but create the `rcx` table. Loading installs no hooks and loads no third-party DLL. The
Direct3D 9 hooks go in and `openvr_api.dll` is loaded only when Lua calls `rcx.vr.Start()`
(`rcx.vr.IsHeadsetPresent()` also loads `openvr_api.dll`, to ask the runtime).

## Lua API

| Function | Does |
|---|---|
| `rcx.Version()` | The version string, with the build time |
| `rcx.Features()` | Feature names to true, for what this build has and isn't switched off: `vr`, `ik` |
| `rcx.Disabled()` | Feature names to the reason each is off (missing dependency, failed start, convar `rcx_<feature>` 0) |
| `rcx.vr.IsHeadsetPresent()` | true, or false and a reason |
| `rcx.vr.Start()`, `rcx.vr.Stop()`, `rcx.vr.IsRunning()` | The session. `Start` returns true, or false and a reason |
| `rcx.vr.GetDisplay()` | `width`, `height` (recommended per eye), `maxWidth`, `maxHeight` (the largest eye texture the headset's driver takes before the compositor downsamples it, from `Prop_Hmd_MaxDistortedTextureWidth_Int32` / `Height`; absent when the driver doesn't set them, since 0.53), `refreshRate`, and `left` / `right`: `tanLeft`, `tanRight`, `tanUp`, `tanDown` (the frustum's extent on each side as a tangent, all positive for a normal eye), `pos` (meters) and `ang`, the eye relative to the head in Source axes |
| `rcx.vr.BeginTarget(width, height)`, `rcx.vr.EndTarget()` | Bracket the `GetRenderTargetEx` call that creates the eye texture. The first render target texture of exactly that size created between the two is captured. A target that already exists isn't created again, so use a new name; a texture captured earlier in the process stays captured |
| `rcx.vr.SetTextureBounds(eye, uMin, vMin, uMax, vMax)` | The part of the eye texture the compositor shows for `"left"` or `"right"` (0 to 1, v down). Defaults: the left and right halves |
| `rcx.vr.WaitFrame()` | Blocks until the compositor wants the next frame, reads the poses, returns a frame id (or nil and a reason) |
| `rcx.vr.GetPoses()` | The poses `WaitFrame` read: `frame`, and `head`, `left`, `right`, each `valid`, `pos` (meters), `ang`, `vel` (meters per second), `angvel` (degrees per second), in Source axes relative to the standing play space |
| `rcx.vr.Submit(frameId)` | Queues the eye texture for the compositor with the head pose of that frame. The submit itself happens at the game's next Present that follows a draw into the eye texture |
| `rcx.vr.GetStats()` | Counters for probes and bug reports: `appKey` and `appIdentity` (what claiming the app key did, since 0.54), `backend`, device found, eye captured, frames submitted and skipped, errors and the last one, the flush or GPU wait at Present, whether Present ran on another thread, `detached` |
| `rcx.vr.RegisterAction(name, kind, binding, label, set)` | Declares an input action: `name` 1 to 32 characters of `a-z0-9_`, starting with a letter and not with `rcx_`; `kind` `"boolean"`, `"vector1"` or `"vector2"`; `binding` (optional) a suggested default, `left_` or `right_` and one of `stick` (vector2), `stick_click`, `a`, `b` (boolean), `trigger`, `grip` (boolean or vector1), and since 0.55 on `a` or `b` for a boolean optionally a press type, `_single`, `_double`, `_long` or `_held` (SteamVR's own button inputs, so one button carries several actions; on a controller where that slot isn't a button, `_single` falls back to the plain click and the others get no default); since 0.57 `both_` and a button slot (`both_stick_click`, a boolean, no press type) is a chord, the slot on both hands at once, written to the set's `chords` in each controller's default bindings; `label` (optional, since 0.54) the name SteamVR's binding screen shows, at most 48 bytes, printable, no quotes or backslashes; `set` (optional, since 0.54) `"main"` (the default), `"pointer"`, `"pipboy"` or `"weapon"`. At most 48. Registering a name again with the same values does nothing; with different ones it moves or relabels it |
| `rcx.vr.SetActionSets(...)` | Writes the action manifest and default bindings (Index, Touch, Vive, Windows Mixed Reality) into `rcx_vr_input/` next to the DLL when they changed, hands OpenVR that path, and makes the named sets the active ones (since 0.54 any of `main`, `pointer`, `pipboy`, `weapon`; none means `main`). The first named has the highest priority: where two active sets bind one input, its action wins and the other's reads inactive. Switching sets is cheap (no rewrite), so call it whenever the game's state changes. Needs a running session; call it again after registering more actions |
| `rcx.vr.GetActions()` | The values `WaitFrame` read: name to a boolean, a number 0 to 1, or `{x, y}` -1 to 1 |
| `rcx.vr.Haptic(hand, seconds, frequency, amplitude)` | A pulse on `"left"` or `"right"`: 0 to 5 s, 0 to 320 Hz, amplitude 0 to 1 |
| `rcx.vr.GetHandSkeleton(hand)` | Finger curl from the controller's skeleton, 0 straight to 1 curled: `thumb`, `index`, `middle`, `ring`, `pinky`; since 0.57 `splay`, the four gaps between fingers (thumb-index first), 0 together to 1 spread; nil and a reason when the controller reports none |
| `rcx.vr.GetPlayArea()` | `width` and `depth` in meters, from the chaperone |
| `rcx.vr.Fade(seconds, r, g, b, a)` | The compositor's fade to a color (0 to 10 s, each 0 to 1); alpha 0 fades back in |
| `rcx.ik.SolveArm(sx, sy, sz, tx, ty, tz, px, py, pz, upper, lower)` | Two-bone arm solve (since 0.53): shoulder, target and a pole point the elbow bends toward, then the upper arm and forearm lengths, any units. Returns seven values, `elbowX, elbowY, elbowZ, handX, handY, handZ, reached`: the hand is the target, or the point nearest it the arm reaches (`reached` false). Numbers must be finite and within 1e6, lengths above 0 and at most 1e4; anything else gives nil and a reason. No table is made either way |
| `rcx.ik.SolveArms(...)` | Both arms in one call: the left arm's eleven arguments, then the right's; fourteen results, the left's seven then the right's |
| `rcx.vr.GetActionOrigins(name)` | How many inputs the binding SteamVR loaded puts on a registered action, 0 when none (since 0.55); nil and a reason before `SetActionSets` or for an unknown name. A binding saved before an action existed leaves it on none |
| `rcx.vr.ShowKeyboard(text, maxCharacters, multiLine, minimal, description)` | Opens SteamVR's system keyboard for the game (since 0.55): `text` the box's current text (at most 4096 bytes), `maxCharacters` 0 to 1024 (0: 1024), `multiLine` a boolean, `minimal` (default true) each key sent as it is typed, `description` a short string. Modal; never password mode. True, or false and a reason |
| `rcx.vr.HideKeyboard()`, `rcx.vr.IsKeyboardOpen()` | Closes it (a `keyboard_closed` event follows); whether it is open |
| `rcx.vr.SetKeyboardPose(x, y, z, yaw, pitch)` | Places the keyboard in SteamVR's standing space: meters (Y up, -Z forward), then yaw about Y and pitch about the keyboard's sideways axis in degrees (since 0.55; not seen in a headset) |
| `rcx.vr.PollEvents()` | The events since the last call, oldest first, each `{type, value}` and for the keyboard `text`: `quit`, `dashboard_open`, `dashboard_close`, `ipd_changed` (value: meters), `play_area_changed`, `controllers_changed`, `input_focus_changed`, `headset_disconnected`; since 0.55 `keyboard_char` (text: what was typed, `"\b"` backspace, `"\n"` enter, an escape sequence such as `"\x1b[D"` for an arrow), `keyboard_done` (text: SteamVR's copy of the text, empty in minimal mode, where it keeps none) and `keyboard_closed` (after done, or canceled). At most 64 are kept |

`rcx.ik` honors its convar `rcx_ik` through `rcx.Features()` only: a pure solver has nothing to protect,
and reading the convar on every call would cost more than the solve.

Every function checks its arguments and returns false or nil and a reason on failure; none raises
a Lua error. Nothing takes or returns a file path, pointer or handle.

## Input

Lua declares actions and suggests a binding for each; the module writes `actions.json` and
`bindings_knuckles.json`, `bindings_oculus_touch.json`, `bindings_vive_controller.json` into the fixed
folder `rcx_vr_input/` next to the DLL (names are checked to `a-z0-9_` and bindings come from a fixed
list, so nothing Lua sends becomes a path). The module adds its own set beside Lua's, `/actions/rcx`, always active at the lowest priority: a
skeleton per hand (`skeleton_left`, `skeleton_right`, read for `GetHandSkeleton`) and one vibration
output (`haptic`). Every action and set has a display name in the manifest's `localization` (`en_US`):
Lua's label, else its name. `WaitFrame` updates the action state after the poses.

**The game's own identity for bindings (since 0.54).** SteamVR keeps bindings per application, and a
game launched by Steam is `steam.app.4000` to it, the same app as every other Garry's Mod VR addon
(VRMod), so their saved bindings could load instead of these (`VR-1`). The first `Start` in a process
writes `rcx_vr_input/resurgence.vrmanifest` (app key `gmod.resurgence.rcx`, its action manifest, the
game's exe path from the process itself), registers it with `IVRApplications::AddApplicationManifest`
as temporary (SteamVR forgets it when it quits, nothing is installed) and claims the key for the
process with `IdentifyApplication(0, key)`, before any action manifest is set. If either call fails,
the session goes on as `steam.app.4000` and `GetStats().appIdentity` says why. Not yet run against
SteamVR (no headset tonight): whether SteamVR lets a process Steam launched take another key is the
thing to check.

**Checked without SteamVR:** every build runs `builds/win64/tests/rcx_input_files_test.exe`, which
registers Resurgence's actions and writes the files, and `tools/check_input_files.py`, which parses
them and checks display names, that bindings name only declared actions, that no input is bound twice
in one set, and the application manifest.

## Session lifetime

The module pins itself once VR starts, and the session outlives the Lua state that started it. When a
client Lua state closes (a disconnect or a server change), the compositor fades to its grid and
rendering is suspended; the next `WaitFrame`, from the next Lua state, resumes rendering and fades the
grid out. `Stop` ends the session. SteamVR quitting (`VREvent_Quit`) stops the session inside
`WaitFrame`, which then returns nil and `SteamVR is quitting`, and `PollEvents` reports `quit`.

## How the picture reaches the headset

1. `rcx.vr.Start()` makes a throwaway Direct3D 9 device from the game's own `d3d9.dll`, takes its
   vtable and patches `Present`, `PresentEx`, `CreateTexture`, `SetRenderTarget`, `Reset` and
   `ResetEx`. The game's device shows up as `this` on its next `Present`. The hooks stay for the
   process's life (the module pins itself) and do nothing while no session runs.
2. `BeginTarget` / `EndTarget` capture the eye texture from the `CreateTexture` hook.
3. `Submit(frameId)` only queues the frame and its head pose. At the game's next `Present` that
   follows a draw into the eye texture (on the render thread with the queued renderer), the bridge
   copies the eye texture into one of two textures of its own and submits that, with the pose.
   The compositor takes one submit per `WaitGetPoses`; a frame that would be a second one is
   skipped (`framesSkipped`).
4. The submit path is chosen at the first submit:
   - **DXVK** (`backend` = `dxvk vulkan interop`). The game's device answers DXVK's
     `ID3D9VkInteropDevice`. The copy's Vulkan image is submitted on DXVK's own queue: transition to
     a transfer layout, `FlushRenderingCommands`, lock DXVK's submission queue, `Submit`,
     `PostPresentHandoff`, `SubmitExplicitTimingData`, unlock, transition back. The session uses
     `Explicit_ApplicationPerformsPostPresentHandoff`, so `WaitGetPoses` on the Lua thread never
     touches DXVK's queue (without it the game crashed inside `WaitGetPoses` within seconds).
     DXVK 3.0.2 can't share a Direct3D 9 texture with Direct3D 11 in either direction, so this is
     the only path there.
   - **Native Direct3D 9** (`backend` = `d3d11 shared texture`). The copy is a texture shared with
     a Direct3D 11 device on the headset's adapter, submitted as a Direct3D 11 texture after an
     event query shows the copy finished. Not run yet: the development machine runs on DXVK.

Lua never waits on the render thread, except `Stop`, which waits for a `Present` in progress.

## Testbed

`lua/autorun/client/rcx_testbed.lua` runs the VR frame loop in sandbox (or any gamemode) on any map:

```
rcx_status
rcx_vr_test_start pattern
rcx_vr_test_mode scene
rcx_vr_test_stop
```

`rcx_status` prints the version, features and stats. The testbed's stop (its command and its `ShutDown` hook) ends only a session the testbed started, so it never ends another addon's session that should outlive the Lua state. `pattern` shows a fixed test card per eye (no
tracking); `scene` renders the world from the headset. `rcx_vr_test_scale` sets world units per
real meter (52.49), `rcx_vr_test_verbose` prints frame timings every 5 seconds. Set
`fps_max_nofocus 0` in the console if the game window won't keep focus (an unfocused game is capped at about 20 FPS; `engine_no_focus_sleep` doesn't exist in Garry's Mod). In single player the
GMod menu pauses the game and with it the frame loop; a listen server with 2 slots doesn't pause.

## Building

Requirements: Visual Studio 2022 or later with the C++ workload (it bundles CMake 3.20+), and two
dependencies in `module/third_party/` (gitignored):

```bash
git clone --depth 1 -b development https://github.com/Facepunch/gmod-module-base.git module/third_party/gmod-module-base
```

```bash
git clone --depth 1 -b v2.15.6 https://github.com/ValveSoftware/openvr.git module/third_party/openvr
```

The development branch of gmod-module-base is required: its master branch has the 32-bit
`lua_State` layout and a 64-bit module built against it crashes on `require`. An OpenVR SDK
elsewhere works too: `-DRCX_OPENVR_DIR=<folder>`, or a junction at `module/third_party/openvr`.

Then run `module/build_module.bat -Install` (`module/README.md` has the steps: the build, the
tests, the lock check, the rollback copy in `lua/bin/rcx_previous/` and `-Rollback`). It builds
`module/builds/win64/gmcl_rcx_win64.dll` and copies `openvr_api.dll` beside it. `-Install` also
copies both into `garrysmod/lua/bin`, which it refuses while a running game has the module loaded
(GMod keeps a loaded module until the process exits, and the module pins itself once VR starts).
`-Clean` rebuilds from scratch. Every build also builds and runs `builds/win64/rcx_ik_test.exe`, the IK
solver's unit tests (`tests/ik_solver_test.cpp`, no game needed), and the input files check above
(Python 3 on the path); a failing check fails the build.

The module links the static C runtime and imports only `kernel32` and `user32`; `d3d9`, `d3d11`,
`dxgi` and `openvr_api` are loaded by hand.

## Installing

`module/build_module.bat -Install`, or copy `gmcl_rcx_win64.dll` and `openvr_api.dll` into `garrysmod/lua/bin`. VR needs SteamVR.

## Version

`kVersion` in `module/src/module.cpp`. Bump it on every build that changes behavior, and name the
change after the number.

## Layout

| Path | What |
|---|---|
| `module/src/module.cpp` | `gmod13_open` / `gmod13_close`, the `rcx` table, `Version`, `Features`, `Disabled`, the convar check |
| `module/src/core/` | Logging, the feature registry, the module folder and pinning; `lua_util.h`, the Lua boundary (`RCX_FUNCTION`, argument checks) |
| `module/src/vr/openvr_loader.*` | Loads `openvr_api.dll` from the module's own folder |
| `module/src/vr/vr_session.*` | The OpenVR session, display, frame wait, poses, events, play area, fade, and keeping the session between Lua states |
| `module/src/vr/d3d9_bridge.*` | The Direct3D 9 vtable hooks, the eye texture capture, the copies and the submit at Present (DXVK and native paths) |
| `module/src/vr/dxvk_interop.h` | DXVK's Direct3D 9 interop interfaces and the few Vulkan types they use (no Vulkan SDK needed) |
| `module/src/vr/vr_input.*` | Actions, the manifest and default bindings, haptics, the finger curl |
| `module/src/vr/vr_lua.*` | The `rcx.vr` table and the OpenVR to Source conversions |
| `module/src/ik/ik_solver.*` | The body IK solver: plain C++, no GMod or OpenVR, so it builds into the unit tests too |
| `module/src/ik/ik_lua.*` | The `rcx.ik` table: argument checks and results on the stack |
| `module/tests/ik_solver_test.cpp` | The solver's unit tests and a timing loop (`rcx_ik_test.exe`) |
| `module/tests/input_files_test.cpp`, `module/tools/check_input_files.py` | The VR input files written without SteamVR, and their check |
| `module/build_module.bat`, `module/tools/build_module.ps1`, `module/README.md` | The build, the install with its lock check and rollback copy, and how to run them |
| `lua/autorun/client/rcx_testbed.lua` | The testbed |

## Acknowledgment

The VR component is written from scratch, but owes its approach to **VRMod** by Catse, and its
fork by Abyss-c0re and Metastruct (zlib-style license): rendering to a headset from
`render.RenderView` in a `RenderScene` hook, sharing the game's Direct3D 9 texture with a
Direct3D 11 device for the compositor, and the frustum and texture bounds approach. None of its
code is copied here. `module/src/vr/dxvk_interop.h` declares DXVK's interop interfaces (zlib
license) as published in DXVK's `src/d3d9/d3d9_interfaces.h`.
