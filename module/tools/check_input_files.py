"""
Checks the VR input files that builds/win64/tests/rcx_input_files_test.exe writes: every file is
valid JSON, every action and set has a display name, each controller's bindings point only at
declared actions, no input is bound twice within one set, and reload and ui_toggle (both on the
right B button) land in different sets. Press types (single, double, long, held) sit only on
"button" mode sources, never beside a plain click, with both press delays set; on the Index and
Touch, W8's layout has its presses (menu and weapon_menu on left A, ui_toggle and
quick_select_right on right B, reload, ammo_next and quick_select_weapon on right B in the weapon set). Chords point only at
declared actions with two inputs, and on the Index and Touch tuning_menu is the chord of both
thumbstick clicks. The headsets with eye tracking (Steam Frame, Quest over Steam Link, other Steam
Link headsets) get bindings holding only the eye gaze, an eyetracking action on
/user/head/eyetracking. Run by tools/build_module.ps1 after the exe.
"""
import json
import os
import sys

folder = os.path.join(sys.argv[1], "rcx_vr_input")
failures = []

def load(name):
    with open(os.path.join(folder, name), encoding="utf-8") as handle:
        return json.load(handle)

manifest = load("actions.json")
actions = {action["name"] for action in manifest["actions"]}
sets = {entry["name"] for entry in manifest["action_sets"]}
names = manifest["localization"][0]

for path in actions | sets:
    if path not in names:
        failures.append("no display name for " + path)

if names.get("/actions/main/in/pipboy") != "Open Pip-Boy":
    failures.append("pipboy's display name is " + repr(names.get("/actions/main/in/pipboy")))

controllers = {"knuckles", "oculus_touch", "vive_controller", "holographic_controller"}
headsets = {"frame_hmd", "rift", "remote_hmd"}
expected = controllers | headsets
listed = {entry["controller_type"] for entry in manifest["default_bindings"]}

if listed != expected:
    failures.append("default bindings for " + str(sorted(listed)))

for entry in manifest["default_bindings"]:
    bindings = load(entry["binding_url"])

    for set_path, set_bindings in bindings["bindings"].items():
        if set_path not in sets:
            failures.append(entry["controller_type"] + ": bindings for an undeclared set " + set_path)

        seen = set()

        for source in set_bindings.get("sources", []):
            for component, output in source["inputs"].items():
                key = (source["path"], component)

                if key in seen:
                    failures.append(entry["controller_type"] + ": " + str(key) + " bound twice in " + set_path)

                seen.add(key)

                if output["output"] not in actions:
                    failures.append(entry["controller_type"] + ": output " + output["output"] + " isn't declared")

                if not output["output"].startswith(set_path + "/"):
                    failures.append(entry["controller_type"] + ": " + output["output"] + " bound under " + set_path)

            components = set(source["inputs"].keys())
            typed = components & {"single", "double", "long", "held"}

            if typed and source["mode"] != "button":
                failures.append(entry["controller_type"] + ": press types on a " + source["mode"] + " source " + source["path"])

            if typed and "click" in components:
                failures.append(entry["controller_type"] + ": a click beside press types on " + source["path"])

            timed = typed & {"double", "long", "held"}
            parameters = source.get("parameters", {})

            if timed and ("long_press_delay" not in parameters or "double_press_delay" not in parameters):
                failures.append(entry["controller_type"] + ": no press delays on " + source["path"])

        for chord in set_bindings.get("chords", []):
            if chord["output"] not in actions:
                failures.append(entry["controller_type"] + ": chord output " + chord["output"] + " isn't declared")

            if len(chord.get("inputs", [])) != 2 or any(len(pair) != 2 for pair in chord["inputs"]):
                failures.append(entry["controller_type"] + ": chord " + chord["output"] + " needs two [path, component] inputs")

        for item in set_bindings.get("skeleton", []) + set_bindings.get("haptics", []) + set_bindings.get("eyetracking", []):
            if item["output"] not in actions:
                failures.append(entry["controller_type"] + ": output " + item["output"] + " isn't declared")

    outputs = json.dumps(bindings)

    if entry["controller_type"] in headsets:
        gaze = bindings["bindings"].get("/actions/rcx", {}).get("eyetracking", [])

        if gaze != [{"output": "/actions/rcx/in/eye_gaze", "path": "/user/head/eyetracking"}]:
            failures.append(entry["controller_type"] + ": the eye gaze binding is " + json.dumps(gaze))

        continue

    if entry["controller_type"] in ("knuckles", "oculus_touch"):
        wanted = {
            "/actions/main/in/menu": "single",
            "/actions/main/in/weapon_menu": "long",
            "/actions/main/in/ui_toggle": "single",
            "/actions/main/in/quick_select_right": "long",
            "/actions/weapon/in/reload": "single",
            "/actions/weapon/in/ammo_next": "double",
            "/actions/weapon/in/quick_select_weapon": "long",
        }
        found = {}

        for set_bindings in bindings["bindings"].values():
            for source in set_bindings.get("sources", []):
                for component, output in source["inputs"].items():
                    found[output["output"]] = component

        for action, component in wanted.items():
            if found.get(action) != component:
                failures.append(entry["controller_type"] + ": " + action + " is on " + repr(found.get(action)) + ", not " + component)

        chords = bindings["bindings"].get("/actions/main", {}).get("chords", [])
        tuning = [chord for chord in chords if chord["output"] == "/actions/main/in/tuning_menu"]
        stick = "/input/thumbstick" if entry["controller_type"] == "knuckles" else "/input/joystick"
        wanted_inputs = [["/user/hand/left" + stick, "click"], ["/user/hand/right" + stick, "click"]]

        if len(tuning) != 1 or tuning[0]["inputs"] != wanted_inputs:
            failures.append(entry["controller_type"] + ": tuning_menu isn't the chord of both thumbstick clicks: " + json.dumps(tuning))

    for action in ("/actions/main/in/ui_toggle", "/actions/weapon/in/reload"):
        if action not in outputs:
            failures.append(entry["controller_type"] + ": " + action + " has no default binding")

app = load("resurgence.vrmanifest")
application = app["applications"][0]

if application["app_key"] != "gmod.resurgence.rcx" or not application["action_manifest_path"].endswith("actions.json"):
    failures.append("application manifest: " + json.dumps(application))

types = {action["name"]: action["type"] for action in manifest["actions"]}

if types.get("/actions/rcx/in/eye_gaze") != "eyetracking":
    failures.append("eye_gaze is a " + repr(types.get("/actions/rcx/in/eye_gaze")) + " action, not eyetracking")

for failure in failures:
    print("FAIL " + failure)

print("input files: %d actions, %d sets, %d controllers checked, %d failures" % (len(actions), len(sets), len(listed), len(failures)))
sys.exit(1 if failures else 0)
