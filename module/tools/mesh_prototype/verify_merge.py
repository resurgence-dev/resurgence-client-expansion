"""Checks each merged model offline against the game's real bone matrices (data/rcx_bake_probe.json): every vertex is
skinned the way the separate part draws it (the part's bones, the part's bind) and the way the merged model draws it
(the body's bones, the merged bind), and the distance between the two is reported per case and per part, with the
source bones of the worst vertices."""
import json, os, struct, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import merge, source_files

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod"
probe = {c["label"]: c for c in json.load(open(os.path.join(GAME, "data", "rcx_bake_probe.json"), encoding="utf-8"))["cases"]}
cases = json.load(open(os.path.join(GAME, "data", "rcx_merge_cases.json"), encoding="utf-8"))["cases"]


def apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) + m[i][3] for i in range(3)]


def skin(raw, bone_world):
    """The skinned world position of a 48-byte VVD vertex; bone_world(index) gives world * poseToBone for a bone index."""
    weights = struct.unpack_from("<3f", raw, 0)
    count = raw[15]
    pos = struct.unpack_from("<3f", raw, 16)
    out = [0.0, 0.0, 0.0]
    total = 0.0
    for k in range(count):
        m = bone_world(raw[12 + k])
        p = apply(m, pos)
        for i in range(3):
            out[i] += weights[k] * p[i]
        total += weights[k]
    return [c / total for c in out] if total else out


def merged_bones(path):
    m = open(path, "rb").read()
    i32 = lambda o: struct.unpack_from("<i", m, o)[0]
    out = []
    for b in range(i32(156)):
        o = i32(160) + b * 216
        name = m[o + i32(o): m.index(b"\0", o + i32(o))].decode("latin1")
        out.append((name.lower(), merge.pose_to_bone(m[o:o + 216])))
    return out


for case in cases:
    if not case.get("merged") or case["label"] not in probe:
        continue
    seen = probe[case["label"]]
    body_world = {b["name"].lower(): [list(r) for r in b["world"]] for b in seen["bodyBones"] if b.get("world")}
    base = os.path.join(GAME, case["merged"].replace("/", os.sep))[:-4]
    mbones = merged_bones(base + ".mdl")
    vvd = open(base + ".vvd", "rb").read()
    merged_world = [merge.m_mul(body_world[name], ptb) if name in body_world else None for name, ptb in mbones]
    cursor = 64
    print("==", case["label"])
    for part in case["parts"]:
        probe_part = next((p for p in seen["parts"] if p["id"] == part["id"]), None)
        spec = dict(source_files.model_files(part["model"]), bodygroups=part.get("bodygroups") or {}, skin=part.get("skin") or 0)
        src = merge.Part(spec)
        if not part.get("bonemerge", True) or probe_part is None:
            for mesh in src.chosen_meshes():
                cursor += 48 * len(mesh["verts"])
            print("  %s: positioned or not probed, skipped" % part["id"])
            continue
        part_world_by_name = {b["name"].lower(): [list(r) for r in b["world"]] for b in probe_part["bones"] if b.get("world")}
        part_world = [merge.m_mul(part_world_by_name[b["name"].lower()], merge.pose_to_bone(b["raw"])) for b in src.bones]
        worst, errors, bad_bones = 0.0, [], Counter()
        for mesh in src.chosen_meshes():
            for raw in mesh["verts"]:
                merged_raw = vvd[cursor:cursor + 48]
                cursor += 48
                a = skin(raw, lambda i: part_world[i])
                b = skin(merged_raw, lambda i: merged_world[i])
                d = sum((a[i] - b[i]) ** 2 for i in range(3)) ** 0.5
                errors.append(d)
                if d > 0.5:
                    for k in range(raw[15]):
                        bad_bones[src.bones[raw[12 + k]]["name"]] += 1
                worst = max(worst, d)
        errors.sort()
        print("  %s %s: %d verts, max %.2f, p99 %.2f, over 0.5: %d; bones of those: %s" % (part["id"], os.path.basename(part["model"]), len(errors), worst,
              errors[int(len(errors) * 0.99)] if errors else 0, sum(1 for e in errors if e > 0.5), bad_bones.most_common(5)))
