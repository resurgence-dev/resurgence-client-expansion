"""Builds one merged model per NPC in data/rcx_merge_dump.json and writes data/rcx_merge_cases.json for
client/mesh_merge_compare: per case the body, the parts that went in (as the compare draws them) and the merged
model, and per part left out the reason."""
import json, math, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import merge, source_files, bake_plan

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod"
OUT = os.path.join(GAME, "models", "rcx_merged")
WEAPON_PARTS = {"wep", "holster"}
BAKE = "--bake" in sys.argv
# --fixtures <dir>: also write each case's inputs and the prototype's output for module/tests/mesh_merge_test.cpp
FIXTURES = sys.argv[sys.argv.index("--fixtures") + 1] if "--fixtures" in sys.argv else None


def write_fixture(name, npc, specs, included, skeleton_bake):
    folder = os.path.join(FIXTURES, name)
    os.makedirs(folder, exist_ok=True)
    def put(file, data):
        with open(os.path.join(folder, file), "wb") as f:
            f.write(data)
    put("body.mdl", files(npc["body"])["mdl"])
    lines = ["name\t" + name]
    for key, target in sorted(skeleton_bake.items()):
        lines.append("bake\t%s\t%s" % (key, target))
    for i, (spec, part) in enumerate(zip(specs, included)):
        put("part%d.mdl" % i, spec["mdl"]); put("part%d.vvd" % i, spec["vvd"]); put("part%d.vtx" % i, spec["vtx"])
        lines.append("part\t%d\t%d\t%s\t%d" % (i, spec["skin"], spec.get("material") or "", 0 if part["bonemerge"] else 1))
        for group, value in sorted((spec.get("bodygroups") or {}).items()):
            lines.append("bodygroup\t%d\t%s\t%d" % (i, group, value))
        for material in spec.get("materials") or []:
            lines.append("material\t%d\t%s" % (i, material))
        if not part["bonemerge"]:
            pos, ang = part.get("Pos") or [0, 0, 0], part.get("Ang") or [0, 0, 0]
            lines.append("place\t%d\t%s\t%s\t%r\t%r\t%r\t%r\t%r\t%r\t%r" % (i, part.get("bone") or "", part.get("attachment") or "",
                         pos[0], pos[1], pos[2], ang[0], ang[1], ang[2], float(part.get("scale") or 1)))
    for ext, out in (("mdl", "mdl"), ("vvd", "vvd"), ("dx90.vtx", "vtx")):
        put("expect." + out, open(os.path.join(OUT, name + "." + ext), "rb").read())
    with open(os.path.join(folder, "case.txt"), "w", encoding="latin1", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

dump = json.load(open(os.path.join(GAME, "data", "rcx_merge_dump.json"), encoding="utf-8"))

# --extra: parts placed on an attachment with an angle offset (backpacks.lua), each with the NCR torso on the trooper's
# fatigues; the dumped loadouts had none drawn in a pose off their bind (the surplus sack's root turns 90 degrees)
EXTRA_PACKS = [
    ("surplussack", "models/vex/fallout76/backpacks/atx_backpack_surplussack.mdl", "chest", [0, 180, 0], [-12.5, 0, -3], 1.15),
    ("vaultsurvivor", "models/vex/fallout76/backpacks/backpack_vault_survivor.mdl", "chest", [0, 180, 0], [-12.5, 0, 0], 1.2),
    ("oguashell", "models/vex/fallout76/backpacks/oguashellbackpack.mdl", "chest", [0, 180, 0], [-12, 0.5, -4], 1.15),
    ("atomicset", "models/vex/fallout76/backpacks/atx_atomicset_backpack.mdl", "WeaponThigh", [-180, -90, -90], [0, 0, -2], 0.9),
    ("cultist", "models/vex/fallout76/backpacks/atx_cultist_backpack.mdl", "chest", [-10, 180, 16], [-10, 1.5, 7], 1.15),
    ("raiderunique", "models/vex/fallout76/backpacks/backpack_raider_unique.mdl", "chest", [-15, 180, 0], [-11.5, 0, -2], 1.33),
]
# the same surplus sack on other bodies (each with its own chest attachment frame), bonemerged torso left out
EXTRA_BODIES = [
    ("raiderunderarmor", "models/fallout_4/armor/male/raiderunderarmor.mdl"),
    ("female_ncr", "models/fallout_4/armor/female/ncrtrooperfatigues.mdl"),
]
if "--extra" in sys.argv:
    for label, body in EXTRA_BODIES:
        dump.setdefault("npcs", []).append({"profile": "sack_" + label, "seed": 0, "body": body, "parts": [
            {"id": "backpack", "model": EXTRA_PACKS[0][1], "bonemerge": False, "attachment": "chest", "Ang": [0, 180, 0], "Pos": [-12.5, 0, -3],
             "scale": 1.15, "Skin": 0, "info": {"loaded": True, "flexes": 0, "translucent": []}},
        ]})
if "--extra" in sys.argv:
    info = {"loaded": True, "flexes": 0, "translucent": []}
    for label, model, attachment, ang, pos, scale in EXTRA_PACKS:
        dump.setdefault("npcs", []).append({"profile": "pack_" + label, "seed": 0, "body": "models/fallout_4/armor/male/ncrtrooperfatigues.mdl", "parts": [
            {"id": "torso", "model": "models/fallout_4/armor/male/NCRGear/torso.mdl", "bonemerge": True, "BodyGroups": {"sash": 1}, "Skin": 0, "info": info},
            {"id": "backpack", "model": model, "bonemerge": False, "attachment": attachment, "Ang": ang, "Pos": pos, "scale": scale, "Skin": 0, "info": info},
        ]})
stamp = time.strftime("%H%M%S")
cases, report = [], []
file_cache = {}

def files(model):
    if model not in file_cache:
        file_cache[model] = source_files.model_files(model)
    return file_cache[model]

def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0) for j in range(4)] for i in range(3)]


def mat_invert_rigid(m):
    r = [[m[j][i] for j in range(3)] for i in range(3)]
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[i] + [t[i]] for i in range(3)]


def angle_matrix(pitch, yaw, roll, pos):
    """Source's AngleMatrix (degrees), with a translation."""
    sp, cp = math.sin(math.radians(pitch)), math.cos(math.radians(pitch))
    sy, cy = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    sr, cr = math.sin(math.radians(roll)), math.cos(math.radians(roll))
    return [[cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy, pos[0]],
            [cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy, pos[1]],
            [-sp, sr * cp, cr * cp, pos[2]]]


body_cache = {}


def body_skeleton(model):
    """A body model's bones (name -> raw struct and poseToBone) and attachments (name -> bone name and local matrix)."""
    if model in body_cache:
        return body_cache[model]
    m = files(model)["mdl"]
    i32 = lambda o: struct.unpack_from("<i", m, o)[0]
    cstr = lambda o: m[o:m.index(b"\0", o)].decode("latin1")
    numbones, boneindex = i32(156), i32(160)
    bones, names = {}, []
    for b in range(numbones):
        o = boneindex + b * 216
        name = cstr(o + i32(o))
        f = struct.unpack_from("<12f", m, o + 96)
        bones[name.lower()] = {"name": name, "raw": m[o:o + 216], "poseToBone": [list(f[0:4]), list(f[4:8]), list(f[8:12])], "index": b, "parent": i32(o + 4)}
        names.append(name)
    attachments = {}
    numatt, attindex = i32(156 + 4 * 21), i32(156 + 4 * 22)
    for a in range(numatt):
        o = attindex + a * 92
        name = cstr(o + i32(o))
        f = struct.unpack_from("<12f", m, o + 12)
        attachments[name.lower()] = {"bone": names[i32(o + 8)], "local": [list(f[0:4]), list(f[4:8]), list(f[8:12])]}
    body_cache[model] = {"bones": bones, "attachments": attachments}
    return body_cache[model]


def rigid_for(part, body):
    """The rigid placement of a positioned part on `body`: its bone, and the matrix from the part's model space to the
    body's bind pose. Mirrors the engine path: an attachment parents to it; otherwise FollowBone on `bone`."""
    skeleton = body_skeleton(body)
    local = angle_matrix(*(part.get("Ang") or [0, 0, 0]), part.get("Pos") or [0, 0, 0])
    scale = float(part.get("scale") or 1)
    if scale != 1:
        local = [[c * scale if j < 3 else c for j, c in enumerate(row)] for row in local]
    if part.get("attachment"):
        attachment = skeleton["attachments"].get(str(part["attachment"]).lower())
        if not attachment:
            raise LookupError("attachment %s not on the body" % part["attachment"])
        bone = skeleton["bones"][attachment["bone"].lower()]
        local = mat_mul(attachment["local"], local)
    elif part.get("bone"):
        bone = skeleton["bones"].get(str(part["bone"]).lower())
        if not bone:
            raise LookupError("bone %s not on the body" % part["bone"])
    else:
        raise LookupError("parented to the root, not to a bone or attachment")
    matrix = mat_mul(mat_invert_rigid(bone["poseToBone"]), local)
    return {"bone_name": bone["name"], "bone_raw": bone["raw"], "matrix": matrix}


def reason_out(part):
    info = part.get("info") or {}
    if not part.get("model"):
        return "no model"
    if part.get("NoDraw") or part.get("nullify"):
        return "not drawn"
    if part.get("weaponItem"):
        return "weapon (a part of weapon item %s: held, holstered or thrown)" % part["weaponItem"]
    if part["id"] in WEAPON_PARTS:
        return "weapon slot %s (no weapon item matched its model)" % part["id"]
    if part.get("ArmsOnly") or part.get("OnlyArms"):
        return "arms-only part"
    if part.get("hasAttachModels"):
        return "carries attachment models"
    if info.get("loaded") is False:
        return "model didn't load in game"
    if (info.get("flexes") or 0) > 0:
        return "has flexes (%d)" % info["flexes"]
    if any(info.get("translucent") or []):
        return "translucent or alpha-tested material"
    return None

for n, npc in enumerate(dump.get("npcs", [])):
    label = "%s_%d" % (npc["profile"], npc["seed"])
    included, specs, left = [], [], []
    for part in npc.get("parts", []):
        why = reason_out(part)
        if why is None:
            try:
                spec = dict(files(part["model"]))
            except FileNotFoundError as missing:
                why = "source file not found offline (%s)" % missing
        if why:
            left.append({"id": part["id"], "model": part.get("model"), "reason": why})
            continue
        if not part.get("bonemerge"):
            try:
                spec["rigid"] = rigid_for(part, npc["body"])
            except (LookupError, FileNotFoundError) as failure:
                left.append({"id": part["id"], "model": part.get("model"), "reason": "positioned part: %s" % failure})
                continue
        groups = {}
        for name, value in (part.get("BodyGroups") or {}).items():
            groups[name] = int(value)
        spec.update({"bodygroups": groups, "skin": int(part.get("Skin") or 0), "material": part.get("Material") or None,
                     "materials": (part.get("info") or {}).get("materials")})
        try:
            merge.Part(spec)
        except ValueError as refused:
            left.append({"id": part["id"], "model": part["model"], "reason": "merger refused: %s" % refused})
            continue
        specs.append(spec)
        included.append({"id": part["id"], "model": part["model"], "bodygroups": groups, "skin": spec["skin"],
                         "material": spec["material"] or "", "scaleLayer": bool(part.get("ScaleLayer")),
                         "bonemerge": bool(part.get("bonemerge")), "bone": part.get("bone"), "attachment": part.get("attachment"),
                         "Pos": part.get("Pos"), "Ang": part.get("Ang"), "scale": part.get("scale")})
    weapons_in = [p["id"] for p in included if any(p["id"] == q["id"] and q.get("weaponItem") for q in npc.get("parts", []))]
    case = {"weaponsIncluded": weapons_in, "weaponsLeft": [l["id"] for l in left if l["reason"].startswith("weapon")], "label": label, "profile": npc["profile"], "seed": npc["seed"], "body": npc["body"], "headHidden": bool(npc.get("headNoDraw")),
            "parts": included, "left": left}
    if specs:
        name = "case%s_%s_%02d_%s" % ("b" if BAKE else "", stamp, n, npc["profile"])
        try:
            bake = None
            if BAKE:
                skeleton = body_skeleton(npc["body"])
                body_raw = {k: v["raw"] for k, v in skeleton["bones"].items()}
                skeleton_key, ancestor = bake_plan.bake_map(body_raw, [merge.Part(spec) for spec in specs])
                bake = {"ancestor": ancestor, "body": body_raw, "rest": merge.body_rest(skeleton["bones"])}
                case["skeleton"] = skeleton_key
            result = merge.merge(specs, OUT, name, bake)
            case["baked"] = len(result.get("baked", []))
            case["merged"] = "models/rcx_merged/%s.mdl" % name
            case["stats"] = {k: result[k] for k in ("bones", "meshes", "vertices", "bytes", "rebinds", "unbakedParts")}
            if FIXTURES:
                write_fixture(name, npc, specs, included, bake_plan.ancestors(case["skeleton"])[0] if BAKE else {})
        except Exception as failure:
            case["mergeError"] = str(failure)
    cases.append(case)
    if case.get("stats"): case["stats"]["baked"] = case.get("baked")
    report.append("%s: %d in%s, %d left out%s" % (label, len(included), (" -> %s" % case.get("stats")) if case.get("stats") else "",
                  len(left), ("; " + "; ".join("%s (%s)" % (l["id"], l["reason"]) for l in left)) if left else ""))

with open(os.path.join(GAME, "data", "rcx_merge_cases.json"), "w", encoding="utf-8") as f:
    json.dump({"cases": cases}, f, indent=1)
print("\n".join(report))
