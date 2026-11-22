"""The static-bone bake map for a body, from the Salem session's merged-bones/plan.json: each baked skin bone's
nearest live ancestor, and each part's own bones (not in the body skeleton) folded into their nearest body bone."""
import json, os

PLAN = os.path.join(r"C:\Users\bizzc\AppData\Local\Claude-Data\claude_pzs8sxrjxfjjc\side-session-notes\local_00edd3f0-4db5-46dc-9c63-3c145f752fc5", "merged-bones", "plan.json")
_plan = None


def plan():
    global _plan
    if _plan is None:
        _plan = json.load(open(PLAN, encoding="utf-8"))
    return _plan


def skeleton_for(body_bone_names):
    """The plan skeleton sharing the most bone names with the body model."""
    names = {n.lower() for n in body_bone_names}
    best, score = None, -1
    for key, skeleton in plan()["skeletons"].items():
        overlap = sum(1 for b in skeleton["bones"] if b["name"].lower() in names)
        if overlap > score:
            best, score = key, overlap
    return best


def ancestors(skeleton_key):
    bones = {b["name"].lower(): b for b in plan()["skeletons"][skeleton_key]["bones"]}
    out = {}
    for key, bone in bones.items():
        if not bone["bake"]:
            continue
        parent = bone["parent"]
        while parent and bones.get(parent.lower(), {}).get("bake"):
            parent = bones[parent.lower()]["parent"]
        if parent:
            out[key] = parent
    return out, bones


def bake_map(body_bones, part_objects):
    """body_bones: {lowercase name: raw struct} of the body model; part_objects: merge.Part list."""
    key = skeleton_for([b for b in body_bones])
    out, skeleton = ancestors(key)
    # only a bone the body has is driven by the body; a skin bone the body lacks is baked like a part's own bone
    out = {name: target for name, target in out.items() if name in body_bones and target.lower() in body_bones}
    for part in part_objects:
        for index, bone in enumerate(part.bones):
            name = bone["name"].lower()
            if name in body_bones:
                continue
            parent = bone["parent"]
            target = None
            while parent >= 0:
                pname = part.bones[parent]["name"].lower()
                if pname in skeleton or pname in body_bones:
                    target = out.get(pname, part.bones[parent]["name"])
                    break
                parent = part.bones[parent]["parent"]
            if target:
                out[name] = target
    return key, out
