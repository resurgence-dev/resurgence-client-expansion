# Resurgence Client Expansion

An optional client binary module for Garry's Mod, `gmcl_rcx_win64.dll`, loaded with
`require("rcx")`. It gives client Lua what Lua can't do on its own. Its first component is VR
(`rcx.vr`): an OpenVR session, head and hand poses, and the game's picture handed to the headset.
The second is a body IK solver (`rcx.ik`), pure math with no engine access. The third is
render-thread and GPU timing (`rcx.gpu`, since 0.61), measurement only. Since 0.62 it carries the
safety layer for components that rely on engine internals (`rcx.engine`), ahead of the first one,
merged meshes (`mesh_combine`, being built).
The designs are `docs/client-expansion-design.md` and `docs/vr-design.md` in the Resurgence
gamemode; this addon is the module and a standalone testbed, and runs in any gamemode.

Windows x64 only (the 64-bit game branch). The game never needs it: Lua checks `rcx` and
`rcx.Features()` and copes with it missing.

## What loading does

Create the `rcx` table and run the engine features' build check (below, Engine features): it reads
`engine.dll`, `datacache.dll` and `studiorender.dll` from the files the game loaded them from, and
`rcx_policy.json` and `rcx_state/` next to the module, and changes nothing. Loading installs no hooks and loads no third-party DLL. The
Direct3D 9 hooks go in only when Lua calls `rcx.vr.Start()` or `rcx.gpu.Start()`, and `openvr_api.dll`
is loaded only by `rcx.vr.Start()`
(`rcx.vr.IsHeadsetPresent()` also loads `openvr_api.dll`, to ask the runtime).

## Lua API

| Function | Does |
|---|---|
| `rcx.Version()` | The version string, with the build time |
| `rcx.Features()` | Feature names to true, for what this build has and isn't switched off: `vr`, `ik` |
| `rcx.Disabled()` | Feature names to the reason each is off (missing dependency, failed start, convar `rcx_<feature>` 0) |
| `rcx.vr.IsHeadsetPresent()` | true, or false and a reason |
| `rcx.vr.Start()`, `rcx.vr.Stop()`, `rcx.vr.IsRunning()` | The session. `Start` returns true, or false and a reason |
| `rcx.vr.GetDisplay()` | `width`, `height` (recommended per eye), `maxWidth`, `maxHeight` (the largest eye texture the headset's driver takes before the compositor downsamples it, from `Prop_Hmd_MaxDistortedTextureWidth_Int32` / `Height`; absent when the driver doesn't set them, since 0.53), `maxTextureWidth`, `maxTextureHeight` (the device's texture limit, `D3DCAPS9`, once the game's device has presented through the hooks; since 0.65), `refreshRate`, and `left` / `right`: `tanLeft`, `tanRight`, `tanUp`, `tanDown` (the frustum's extent on each side as a tangent, all positive for a normal eye), `pos` (meters) and `ang`, the eye relative to the head in Source axes |
| `rcx.vr.BeginTarget(width, height)`, `rcx.vr.EndTarget()` | Bracket the `GetRenderTargetEx` call that creates the eye texture. The first render target texture of exactly that size created between the two is captured. A target that already exists isn't created again, so use a new name; a texture captured earlier in the process stays captured. When nothing was captured, `EndTarget`'s reason says why (since 0.65): no render target made at all, Direct3D refusing one (its size, format and `HRESULT`), or one of another size, with the device's texture limit |
| `rcx.vr.SetTextureBounds(eye, uMin, vMin, uMax, vMax)` | The part of the eye texture the compositor shows for `"left"` or `"right"` (0 to 1, v down). Defaults: the left and right halves |
| `rcx.vr.WaitFrame()` | Blocks until the compositor wants the next frame, reads the poses, returns a frame id (or nil and a reason) |
| `rcx.vr.GetPoses()` | The poses `WaitFrame` read: `frame`, and `head`, `left`, `right`, each `valid`, `pos` (meters), `ang`, `vel` (meters per second), `angvel` (degrees per second), in Source axes relative to the standing play space |
| `rcx.vr.Submit(frameId)` | Queues the eye texture for the compositor with the head pose of that frame. The submit itself happens at the game's next Present that follows a draw into the eye texture |
| `rcx.vr.GetStats()` | Counters for probes and bug reports: `appKey` and `appIdentity` (what claiming the app key did, since 0.54), `backend`, device found, eye captured, frames submitted and skipped, errors and the last one, the flush or GPU wait at Present, whether Present ran on another thread, `detached` |
| `rcx.vr.RegisterAction(name, kind, binding, label, set, overrides)` | Declares an input action: `name` 1 to 32 characters of `a-z0-9_`, starting with a letter and not with `rcx_`; `kind` `"boolean"`, `"vector1"` or `"vector2"`; `binding` (optional) a suggested default, `left_` or `right_` and one of `stick` (vector2), `stick_click`, `a`, `b` (boolean), `trigger`, `grip` (boolean or vector1), and since 0.55 on `a` or `b` for a boolean optionally a press type, `_single`, `_double`, `_long` or `_held` (SteamVR's own button inputs, so one button carries several actions; on a controller where that slot isn't a button, `_single` falls back to the plain click and the others get no default); since 0.57 `both_` and a button slot (`both_stick_click`, a boolean, no press type) is a chord, the slot on both hands at once, written to the set's `chords` in each controller's default bindings; `label` (optional, since 0.54) the name SteamVR's binding screen shows, at most 48 bytes, printable, no quotes or backslashes; `set` (optional, since 0.54) `"main"` (the default), `"pointer"`, `"pipboy"` or `"weapon"`; `overrides` (optional, since 0.69) a table of controller type to that controller's own default in place of `binding`, in its own input names. Only `frame_controller` takes one; its inputs are `stick`, `stick_click`, `trigger`, `grip` and `bumper` on both hands, `a`, `b`, `x`, `y` and `menu` on the right, `view` and `dpad_up`, `dpad_right`, `dpad_down`, `dpad_left` on the left (never `system`, SteamVR's). A value is `""` (no default there), `<hand>_<input>` with an optional press type on a button (`"left_view_long"`), or `<hand>_<input>+<hand>_<input>`, a chord of two buttons' clicks for a boolean action (`"left_dpad_up+left_dpad_left"`); an action without one follows `binding` there, and on the Frame the left `a` and `b` slots are empty, so an action on them needs one. Example: `rcx.vr.RegisterAction("hotkey_1", "boolean", nil, "Hotkey 1", "main", {frame_controller = "left_dpad_up+left_dpad_left"})`. At most 48. Registering a name again with the same values does nothing; with different ones it moves or relabels it |
| `rcx.vr.SetActionSets(...)` | Writes the action manifest and default bindings (Index, Touch, Vive, Windows Mixed Reality, since 0.66 the Steam Frame controllers (`frame_controller`), and since 0.60 the eye gaze alone for the Steam Frame and Steam Link headsets) into `rcx_vr_input/` next to the DLL when they changed, hands OpenVR that path, and makes the named sets the active ones (since 0.54 any of `main`, `pointer`, `pipboy`, `weapon`; none means `main`). The first named has the highest priority: where two active sets bind one input, its action wins and the other's reads inactive. Switching sets is cheap (no rewrite), so call it whenever the game's state changes. Needs a running session; call it again after registering more actions |
| `rcx.vr.GetActions()` | The values `WaitFrame` read: name to a boolean, a number 0 to 1, or `{x, y}` -1 to 1 |
| `rcx.vr.Haptic(hand, seconds, frequency, amplitude)` | A pulse on `"left"` or `"right"`: 0 to 5 s, 0 to 320 Hz, amplitude 0 to 1 |
| `rcx.vr.GetHandSkeleton(hand)` | Finger curl from the controller's skeleton, 0 straight to 1 curled: `thumb`, `index`, `middle`, `ring`, `pinky`; since 0.57 `splay`, the four gaps between fingers (thumb-index first), 0 together to 1 spread; nil and a reason when the controller reports none |
| `rcx.vr.GetTipOffset(hand)` | SteamVR's pointing pose for `"left"` or `"right"` (since 0.59): the controller render model's `tip` component (`IVRRenderModels::GetComponentStateForDevicePath`, its `mTrackingToComponentLocal`; what `/user/hand/*/pose/tip` is bound to), relative to the controller pose `GetPoses` gives. Two values: a Vector in meters (forward, left, up, the same axes as the pose) and an Angle (the tip's forward is where it points), for `LocalToWorld(tipPos, tipAng, handPos, handAng)` (scale the Vector first if the pose is scaled). nil and a reason with no session, no controller in that hand, or a model without a tip. Read when the controller changes, not every frame: the module caches it per hand and reads it again when the role's device or an event says the controller changed (activated, updated, deactivated, its role, `Prop_RenderModelName_String` or `Prop_ControllerType_String`); a model whose tip can't be read yet is asked again at most once a second. A tip more than 0.5 m from the controller, or not finite, counts as none. Valve Index (`{indexcontroller}valve_controller_knu_1_0_right`): (-0.02, 0.006, -0.015) m, (40, 5, 0) degrees; the left mirrors it |
| `rcx.vr.GetControllerType(hand)` | The controller in that hand (since 0.59): two strings, its type (`Prop_ControllerType_String`: `knuckles`, `oculus_touch`, `frame_controller`, `vive_controller`, `holographic_controller`, ...; a controller type with no default bindings here is emulated by SteamVR as one that has them and reports that type, as the Steam Frame's did as `oculus_touch` before 0.66) and its render model name (`Prop_RenderModelName_String`), either empty when the driver doesn't say; nil and a reason with no session or no controller. Cached with the tip |
| `rcx.vr.GetEyeGaze()` | The headset's eye gaze (since 0.60), from the module's `eye_gaze` action (type `eyetracking`, in the module's `/actions/rcx` set) bound to `/user/head/eyetracking` in the default bindings for the headsets, `frame_hmd` (Steam Frame), `rift` (Quest and Quest Pro over Steam Link) and `remote_hmd` (other Steam Link headsets), read each `WaitFrame` with `GetEyeTrackingDataForNextFrame` in the standing space. Four values: the gaze direction relative to the head (a unit Vector, forward, left, up), the gaze origin and its fixation point in the tracking space (Vectors in meters, the axes `GetPoses` uses) and whether the eyes are tracked now (false through a blink). nil and a reason when the action isn't active (no eye tracker, or Steam Link not sharing: SteamVR Settings, Steam Link, Share eye tracking data to other apps on this PC), the data isn't valid or there's no head pose |
| `rcx.vr.GetPlayArea()` | `width` and `depth` in meters, from the chaperone |
| `rcx.vr.Fade(seconds, r, g, b, a)` | The compositor's fade to a color (0 to 10 s, each 0 to 1); alpha 0 fades back in |
| `rcx.ik.SolveArm(sx, sy, sz, tx, ty, tz, px, py, pz, upper, lower)` | Two-bone arm solve (since 0.53): shoulder, target and a pole point the elbow bends toward, then the upper arm and forearm lengths, any units. Returns seven values, `elbowX, elbowY, elbowZ, handX, handY, handZ, reached`: the hand is the target, or the point nearest it the arm reaches (`reached` false). Numbers must be finite and within 1e6, lengths above 0 and at most 1e4; anything else gives nil and a reason. No table is made either way |
| `rcx.ik.SolveArms(...)` | Both arms in one call: the left arm's eleven arguments, then the right's; fourteen results, the left's seven then the right's |
| `rcx.vr.GetActionOrigins(name)` | How many inputs the binding SteamVR loaded puts on a registered action, 0 when none (since 0.55), and since 0.68 a second result: a list with one table per input, `{hand, name, shortName, controllerType, devicePath, inputPath, mode, slot, sourceType}`. `hand` is `left`, `right` or empty; `name` is SteamVR's localized name with the hand, controller type and input source (`GetOriginLocalizedName`, all sections), `shortName` the input source alone ("A Button"); `controllerType` the device's `Prop_ControllerType_String`; the paths, mode, slot and source type come from `GetActionBindingInfo`, paired with the input by hand (else by order when the counts agree) and empty when it gives none. Read once per action and kept (since 0.70 only a read with at least one input: an empty one is asked again at the next call) until SteamVR reloads the bindings (`VREvent_ActionBindingReloaded`, `VREvent_Input_BindingLoadSuccessful`, then a `bindings_changed` event) or a controller changes, so a rebind in SteamVR shows at the next call. nil and a reason before `SetActionSets` or for an unknown name. A binding saved before an action existed leaves it on none |
| `rcx.vr.ShowActionOrigins(name)` | Since 0.68: highlights the action's inputs in SteamVR's overlay (`IVRInput::ShowActionOrigins`); true, or nil and a reason |
| `rcx.vr.ShowKeyboard(text, maxCharacters, multiLine, minimal, description)` | Opens SteamVR's system keyboard for the game (since 0.55): `text` the box's current text (at most 4096 bytes), `maxCharacters` 0 to 1024 (0: 1024), `multiLine` a boolean, `minimal` (default true) each key sent as it is typed, `description` a short string. Modal; never password mode. True, or false and a reason |
| `rcx.vr.HideKeyboard()`, `rcx.vr.IsKeyboardOpen()` | Closes it (a `keyboard_closed` event follows); whether it is open |
| `rcx.vr.SetKeyboardPose(x, y, z, yaw, pitch)` | Places the keyboard in SteamVR's standing space: meters (Y up, -Z forward), then yaw about Y and pitch about the keyboard's sideways axis in degrees (since 0.55; not seen in a headset) |
| `rcx.vr.PollEvents()` | The events since the last call, oldest first, each `{type, value}` and for the keyboard `text`: `quit`, `dashboard_open`, `dashboard_close`, `ipd_changed` (value: meters), `play_area_changed`, `controllers_changed`, `input_focus_changed`, `headset_disconnected`; since 0.68 `bindings_changed` (SteamVR loaded new bindings for the game: read `GetActionOrigins` again); since 0.55 `keyboard_char` (text: what was typed, `"\b"` backspace, `"\n"` enter, an escape sequence such as `"\x1b[D"` for an arrow), `keyboard_done` (text: SteamVR's copy of the text, empty in minimal mode, where it keeps none) and `keyboard_closed` (after done, or canceled). At most 64 are kept |

## GPU timing

`rcx.gpu` (since 0.61) times each frame where the game presents it, through the same Direct3D 9
hooks as VR (no engine internals), so harness tests can tell whether the render thread or the GPU
is the limit (`docs/rcx-engine-hooks-design.md` in the gamemode, candidate 12). A frame runs from
one Present's return to the next Present's call, on the thread that presents: the render thread
with `mat_queue_mode 2`. Times are milliseconds. Its convar is `rcx_gpu`.

| Call | Does |
|---|---|
| `rcx.gpu.Start()` | Installs the Direct3D 9 hooks if they aren't in yet and starts timing; clears the window. True, or false and a reason (the convar, no Direct3D 9) |
| `rcx.gpu.Stop()` | Stops timing; the queries are released at the next Present, on the device's thread |
| `rcx.gpu.IsRunning()` | Whether it is timing |
| `rcx.gpu.GetLast()` | The last frame: `frame` (its number), `presentInterval`, `presentBlock`, `renderWall`, `renderBusy`, and the last GPU frame read, `gpuFrame`, `gpuSpan`, `gpuInterval` (a few frames behind `frame`) |
| `rcx.gpu.BeginWindow()` | Clears the window's samples |
| `rcx.gpu.GetWindow()` | Since `BeginWindow` (or `Start`): `seconds`, `frames`, `gpuFrames`, `gpuDropped` (sets reused before the GPU reached them), `disjoint`, `truncated` (past 20,000 samples a metric), and per metric a `{count, mean, p50, p95, max}` table |
| `rcx.gpu.GetStatus()` | `running`, `deviceFound`, `queriesReady`, `queryError` when the device has no timestamp queries, `presentOnMainThread`, `busyCalibrated`, `cyclesPerMs`, `frames`, `gpuFrames`, `gpuDropped`, `disjoint`, `resets`, `threadChanges` |

The metrics:

- `presentInterval`: from one Present call to the next, the rate frames go out.
- `presentBlock`: time inside the real Present; long when the GPU or the swap chain holds the frame back.
- `renderWall`: from a Present's return to the next Present's call: the presenting thread's work plus its waits for the main thread's queue.
- `renderBusy`: the presenting thread's own CPU time over `renderWall` (`QueryThreadCycleTime`, converted with the TSC rate measured against the performance counter over the first 250 ms), so `renderWall - renderBusy` is about what it waited.
- `gpuSpan`: GPU timestamps at the frame's first and last command (`D3DQUERYTYPE_TIMESTAMP`, inside a `TIMESTAMPDISJOINT` pair), read without a flush, usually 2-3 frames late. It counts any time the GPU sat idle between them, so it is the GPU's frame only when the GPU is the limit.
- `gpuInterval`: from one frame's first GPU timestamp to the next one's.

Reading them: the GPU is the limit when `presentBlock` is long and `gpuSpan` is close to
`gpuInterval`; the render thread is when `renderBusy` is close to `presentInterval`; the main
thread is when `renderWall` is much longer than `renderBusy` (the render thread waits for work).
A thread change (the device presented from another thread) or a device reset starts the counts
again. Eight query sets are in flight at most; a GPU further behind than that drops a frame's
timing (`gpuDropped`).

`rcx.ik` honors its convar `rcx_ik` through `rcx.Features()` only: a pure solver has nothing to protect,
and reading the convar on every call would cost more than the solve.

Every function checks its arguments and returns false or nil and a reason on failure; none raises
a Lua error. Nothing takes or returns a file path, pointer or handle.

## Input

Lua declares actions and suggests a binding for each; the module writes `actions.json` and
`bindings_knuckles.json`, `bindings_oculus_touch.json`, `bindings_frame_controller.json` (since 0.66; since 0.69 the Frame's left `a` and `b` slots are empty and its defaults come from the `frame_controller` overrides, so its pad holds only what an override puts there), `bindings_vive_controller.json`, `bindings_holographic_controller.json` into the fixed
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

**Caveat: `GetActionOrigins` doesn't see every bound input.** On the Steam Frame (0.69/0.70,
2026-11-23, Bizz's default bindings loaded per `vrserver.txt`) SteamVR gave no origins, again on
every ask, for every action bound to a press type (`single`, `long`, `double` on a button source)
and for `use`, the left trigger's `click` beside `trigger_left`'s `pull`. Plain clicks and the
thumbstick's position and click were reported. So a zero from `GetActionOrigins` doesn't mean
unbound: the game's warning leaves press types out and counts an action bound when another action
on the same default input reads bound (`Input.UnboundActions`, VR-36), and button prompts can't
rely on it for those actions either. Whether `GetActionBindingInfo` lists them is untested (the
module reads it only when there are origins), and the offline file checks can't see any of this.

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

`module/build_module.bat -Install`, or copy `gmcl_rcx_win64.dll` and `openvr_api.dll` (from a
build or a [release](https://github.com/resurgence-dev/resurgence-client-expansion/releases)) into
`garrysmod/lua/bin`. VR needs SteamVR.

## Releases

Releases are made by hand with `scripts/release.ps1`, which runs every step and stops at the first
one that fails:

```powershell
./scripts/release.ps1
```

`-DryRun` does the checks and the build and shows the plan, and changes nothing in git or on
GitHub. `-Clean` builds from an empty CMake tree. It works from any folder, under PowerShell 7 and
Windows PowerShell 5. It needs the GitHub CLI, set up once: `winget install GitHub.cli`, then
`gh auth login` in a new terminal. `gh` keeps the sign-in itself; the script never sees a token.

1. **Checks** a branch is checked out and not behind `origin`, `gh` is installed and signed in, and
   no workflow in `.github/workflows` starts on a tag push (The release workflow, below).
2. **Shows** `kVersion`, the newest `vX.YY` tag, the branch and the uncommitted files, then **asks**
   for the version and a commit message. Enter takes the version `module.cpp` already has when it is
   newer than the newest tag, the usual case since `kVersion` is bumped as the module changes. A
   version that isn't newer than the newest tag, is older than `kVersion` or whose tag exists is
   refused.
3. **Writes** the version into `kVersion` in `module/src/module.cpp` when it differs. That is the
   only place the addon holds it.
4. **Builds** with `module/tools/build_module.ps1`, without `-Install`, so `garrysmod/lua/bin` is
   never touched and a running game doesn't matter. The build's own tests run as always.
5. **Checks the built DLL** reports the version (the first `rcx X.YY (` in the file), the check the
   launcher's `publish-manifest.ps1` makes on a release.
6. **Shows the plan**: the files to commit, the branch, the tag, and both DLLs with size and
   SHA-256, then asks one y/n. On `n` or `-DryRun`, `module.cpp` is put back as it was.
7. **Commits** everything in the working tree (`git add -A`), **pushes** the branch, creates the
   annotated tag `vX.YY` with the message `rcx X.YY` and pushes it.
8. **Creates the release** with `gh release create`: both DLLs from `module/builds/win64`, titled
   "RCX Alpha X.YY", a pre-release while the version is below 1.0, with the install steps from
   `.github/release-install.md` and GitHub's generated notes. It then reads the release back and
   compares each file's size and SHA-256 with the build.
9. **Offers** to run the launcher's `scripts/publish-manifest.ps1 -RcxTag vX.YY` (`-LauncherRepo`
   names the launcher's folder, default `C:\Users\bizzc\source\repos\resurgence-launcher`). Players
   get the build only once the manifest is published.

If a step after the y/n fails, the script says which steps were done; finish the rest by hand.

### The release workflow

`.github/workflows/release.yml` builds and publishes a release on GitHub. It is not the release
path: it starts only by hand (`workflow_dispatch`), never on a tag push, because a second build
uploaded over the script's files would change the hashes the launcher's manifest holds. The script
refuses to release while any workflow there starts on a tag push. A manual run is for a tag that
has no release yet (a cloud build instead of a local one): on GitHub, Actions, release, Run
workflow, and pick the `vX.YY` tag as the ref. A run on a branch fails the tag check, and a run on
a tag whose release exists fails without touching its files. GitHub offers the Run workflow button
only once the workflow file is on the default branch, `master`.

The repository is public, so the workflow keeps the token's rights small:

- Only a manual run by someone with write access starts it, never a push, a pull request or
  another workflow, so a fork can't reach the write token.
- The token is read-only (`contents: read`) except in the `release` job, which gets
  `contents: write` and runs none of the repository's code: it only downloads the build job's files
  and calls `gh`.
- No secret but `GITHUB_TOKEN` is used, and every action is pinned to a commit (the version is in
  a comment beside it).

The `build` job runs on `windows-latest` and:

- **Checks the tag against `kVersion`.** Tag `vX.YY` needs `kVersion = "rcx X.YY (` in
  `module/src/module.cpp`; any other tag shape or a mismatch fails the run before the build.
- **Fetches the two dependencies at pinned commits,** so a moving upstream can't change a release:
  the OpenVR SDK at `0924064` (tag v2.15.6, only `headers/` and `bin/win64/`, since the samples'
  paths are too long for Windows) and gmod-module-base at `e0156e5` (the development branch, see
  Building). The commits are in the workflow's `env`; update them there with the clone commands
  above.
- **Builds and checks** with `module/tools/build_module.ps1 -Clean`, no `-Install`, so nothing
  touches a GMod folder: Release, x64, MSVC through Visual Studio's CMake, then the IK tests, the
  input-file test and `tools/check_input_files.py`. Any failure stops the release.
- **Hands on** both DLLs and `.github/release-install.md` as a workflow artifact (kept 7 days).

The `release` job, on `ubuntu-latest`, then:

- **Publishes** `gmcl_rcx_win64.dll` and `openvr_api.dll` as the release for the tag, titled
  "RCX Alpha X.YY", a pre-release while the version is below 1.0, with the install steps from
  `.github/release-install.md` followed by GitHub's generated notes. If the release already exists
  (made by the script, or by hand as v0.60 was), the job fails and leaves it as it is.

## Engine features

A component that relies on engine internals (a class layout, the model format the engine loads)
is an engine feature: a wrong guess is a crash, not an error, so it stays off unless this exact game
build was tested (`docs/rcx-engine-hooks-design.md` in the gamemode, The safety layer). The first is
`mesh_combine` (merged meshes), registered since 0.62 and not built yet. At every load the module:

1. **Fingerprints the game DLLs the feature names**, from the files the game loaded them from (never a
   path from Lua): the PE `TimeDateStamp`, `SizeOfImage` and a SHA-256 of `.text`. The feature is
   cleared only when every one matches a clearance row compiled into `src/engine/engine_guard.cpp`
   (`kClearances`); otherwise `rcx.Disabled()` says `unknown game build (...)` with the build key.
   After a game update, fingerprint the new DLLs, test the feature on the harness, add the rows and
   ship a new build.
2. **Reads the launcher's policy file**, `lua/bin/rcx_policy.json` (Lua can't write there):
   `"disabled"` (feature names, or `all`) and `"revoked"` (build keys) turn features off.
3. **Checks the crash latch**: a feature arms `rcx_state/armed_<feature>` before it first touches the
   engine, and the module's clean close removes it. A file left by a crash keeps the feature off
   until `rcx.engine.ClearLatch(feature)` (the player's options), from the next load.

On top, the player's `rcx_<feature>` convar turns it off as for every component, and any Lua may
turn one off for the rest of its Lua state (never on). The check runs again at each load, since a
pinned DLL outlives the Lua state.

| Call | Returns |
|---|---|
| `rcx.engine.GetBuilds()` | `{[feature] = {key, cleared, latched, disabled, dlls = {{name, timeDateStamp, sizeOfImage, textHash, error}}}}` |
| `rcx.engine.Disable(feature, reason)` | true; off for this Lua state. False and a reason for a name that isn't an engine feature |
| `rcx.engine.ClearLatch(feature)` | true when the latch file is gone (on from the next load); false while the feature is in use |

`module/tests/engine_guard_test.cpp` (`rcx_engine_guard_test.exe`, run by every build where the game
is installed) maps the game's DLLs without running them and checks the refusals, the clearance, the
policy file, revocation and the latch; `tests/client/rcx_engine_guard.lua` in the gamemode checks the
build check and the convar in the game.

## Merged meshes

`rcx.mesh` (since 0.63) builds `mesh_combine`'s merged models: one real model per character outfit, from the part
models' bytes, written by a worker thread into the fixed folder `garrysmod/models/rcx_merged/<name>` (`.mdl`, `.vvd`,
`.dx90.vtx`, each through a temporary name, the `.mdl` last). The gamemode's `cl_mergedparts.lua` decides what merges
and draws the result (gamemode `docs/mesh-combine-design.md`). Needs `mesh_combine` on (Engine features); the first
`Build` of a session arms its crash latch and prunes the folder to 512 MB.

| Call | Returns |
|---|---|
| `rcx.mesh.Build(job)` | a build id, or false and a reason. `job = {name = 32 hex digits, body = body .mdl bytes, parts = {{mdl, vvd, vtx = bytes, skin, material, materials = {...}, bodygroups = {[name] = n}, positioned, bone, attachment, pos = {x, y, z}, ang = {p, y, r}, scale}, ...}, bake = {[bone] = ancestor}}` |
| `rcx.mesh.Poll()` | finished builds: `{{id, ok, ms, path, bones, baked, rebinds, meshes, vertices}` or `{id, ok = false, ms, error}}` |
| `rcx.mesh.Cancel(id)` | true when a queued or running build was dropped |
| `rcx.mesh.Exists(name)` | whether the three files are already in the folder (an earlier session's) |
| `rcx.mesh.Stats()` | `{queued, building, done, failed, canceled}` |

The builder (`src/mesh/mesh_merge.cpp`) is a port of the Python prototype in `module/tools/mesh_prototype/`
(`merge.py`, with `bake_plan.py` and `merge_cases.py`), kept byte-identical to it: `merge_cases.py --bake --fixtures
builds/win64/tests/mesh_fixtures` writes real cases' inputs and outputs, and `rcx_mesh_merge_test.exe` (run by every
build where the fixtures exist) compares, then checks refusals of hostile input. One merged bone per name and bind
pose: a part keeps its own bind, a baked bone becomes its live ancestor with the bind that reproduces it, and same-named
bones are all driven by bonemerge; no skinned vertex moves. A positioned part's vertices move onto its one bone.

## Memory

`rcx.sys` (since 0.67) reads memory for Lua's budgets, such as the merged-mesh gate (no new merged model while
the machine is short of RAM or VRAM). No feature switch: it only reads.

| Function | What |
|---|---|
| `rcx.sys.GetMemory()` | A table in MB: `workingSetMB`, `peakWorkingSetMB`, `privateMB` (the game process, `K32GetProcessMemoryInfo`), `pageFaults` (the count) and `pageFaultsPerSec` (since the last call, soft and hard faults both), `totalPhysicalMB`, `availablePhysicalMB`, `memoryLoad` (percent), `commitMB`, `commitLimitMB` (`GlobalMemoryStatusEx`), and `vramValid`: true with `adapter`, `vramDedicatedMB`, `vramBudgetMB`, `vramUsageMB`, `sharedBudgetMB`, `sharedUsageMB` (`IDXGIAdapter3::QueryVideoMemoryInfo`, WDDM's budget and this process's usage, the game's Direct3D 9 allocations included, on the adapter with the most dedicated memory), else `vramReason`. nil and a reason when the process numbers can't be read |

`dxgi.dll` is loaded from System32 at the first call. `rcx_sys_memory_test.exe`, run by every build, checks the
numbers are sane on the build machine and prints them.

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
| `module/src/vr/d3d9_bridge.*` | The Direct3D 9 vtable hooks, the eye texture capture, the copies and the submit at Present (DXVK and native paths), and the Present observer other components (`rcx.gpu`) use |
| `module/src/vr/dxvk_interop.h` | DXVK's Direct3D 9 interop interfaces and the few Vulkan types they use (no Vulkan SDK needed) |
| `module/src/vr/vr_input.*` | Actions, the manifest and default bindings, haptics, the finger curl |
| `module/src/vr/vr_lua.*` | The `rcx.vr` table and the OpenVR to Source conversions |
| `module/src/ik/ik_solver.*` | The body IK solver: plain C++, no GMod or OpenVR, so it builds into the unit tests too |
| `module/src/ik/ik_lua.*` | The `rcx.ik` table: argument checks and results on the stack |
| `module/src/gpu/gpu_timing.*` | `rcx.gpu`'s timing: the Present observer, the timestamp query ring, the samples and summaries |
| `module/src/gpu/gpu_lua.*` | The `rcx.gpu` table |
| `module/src/engine/engine_guard.*` | The engine features' safety layer: DLL fingerprints, the compiled clearances, the policy file, the crash latch |
| `module/src/engine/engine_lua.*` | The `rcx.engine` table and the engine feature list (`RegisterFeatures`) |
| `module/src/mesh/mesh_merge.*` | The merged-mesh builder: parse, bone union and bake, positioned parts, strips, the studio header |
| `module/src/mesh/mesh_worker.*` | The build queue, its worker thread and the file writes into `models/rcx_merged/` |
| `module/src/mesh/mesh_lua.*` | The `rcx.mesh` table |
| `module/tests/mesh_merge_test.cpp`, `module/tools/mesh_prototype/` | The golden test against the Python prototype, and the prototype |
| `module/tests/engine_guard_test.cpp` | The safety layer's checks against the game's DLLs outside the game (`rcx_engine_guard_test.exe`) |
| `module/tests/ik_solver_test.cpp` | The solver's unit tests and a timing loop (`rcx_ik_test.exe`) |
| `module/tests/input_files_test.cpp`, `module/tools/check_input_files.py` | The VR input files written without SteamVR, and their check |
| `module/build_module.bat`, `module/tools/build_module.ps1`, `module/README.md` | The build, the install with its lock check and rollback copy, and how to run them |
| `scripts/release.ps1` | The release script (Releases) |
| `lua/autorun/client/rcx_testbed.lua` | The testbed |

## Acknowledgment

The VR component is written from scratch, but owes its approach to **VRMod** by Catse, and its
fork by Abyss-c0re and Metastruct (zlib-style license): rendering to a headset from
`render.RenderView` in a `RenderScene` hook, sharing the game's Direct3D 9 texture with a
Direct3D 11 device for the compositor, and the frustum and texture bounds approach. None of its
code is copied here. `module/src/vr/dxvk_interop.h` declares DXVK's interop interfaces (zlib
license) as published in DXVK's `src/d3d9/d3d9_interfaces.h`.
