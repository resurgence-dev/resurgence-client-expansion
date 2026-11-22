"""Prototype merger for rcx mesh_combine (docs/mesh-combine-design.md, stage 2).

Merges part models (MDL v48, VVD v4, VTX v7) into one model: a flat union of bones by name, one bodypart with one
submodel holding every chosen submodel's meshes, the chosen skin baked into the texture list, one empty idle
sequence, LOD0 only. merge(parts, out_dir, name) where each part is a dict: mdl, vvd, vtx (bytes), bodygroups
{name: value}, skin, materials (resolved material paths in texture order, optional), material (override, optional).
"""
import hashlib
import math
import os
import struct

BONE_SIZE = 216
MODEL_SIZE = 148
MESH_SIZE = 116
TEXTURE_SIZE = 64
VERTEX_SIZE = 48
TANGENT_SIZE = 16
HEADER_SIZE = 408
HEADER2_SIZE = 256


def i32(data, offset):
    return struct.unpack_from("<i", data, offset)[0]


def cstr(data, offset):
    end = data.index(b"\0", offset)
    return data[offset:end].decode("latin1")


class Part:
    def __init__(self, spec):
        self.spec = spec
        m = spec["mdl"]
        if m[:4] != b"IDST" or i32(m, 4) != 48:
            raise ValueError("not an MDL v48")
        self.m = m
        self.v = spec["vvd"]
        self.x = spec["vtx"]
        if self.v[:4] != b"IDSV" or i32(self.v, 4) != 4:
            raise ValueError("not a VVD v4")
        if i32(self.x, 0) != 7:
            raise ValueError("not a VTX v7")
        h = lambda k: i32(m, 156 + 4 * k)
        self.numbones, self.boneindex = h(0), h(1)
        self.numtextures, self.textureindex = h(12), h(13)
        self.numcdtextures, self.cdtextureindex = h(14), h(15)
        self.numskinref, self.numskinfamilies, self.skinindex = h(16), h(17), h(18)
        self.numbodyparts, self.bodypartindex = h(19), h(20)
        self.numflexdesc = h(26)
        self.numincludemodels = h(45)
        self.flags = i32(m, 152)
        self.vectors = struct.unpack_from("<18f", m, 80)
        self.bones = []
        for b in range(self.numbones):
            o = self.boneindex + b * BONE_SIZE
            raw = bytearray(m[o:o + BONE_SIZE])
            name = cstr(m, o + i32(m, o))
            parent = i32(m, o + 4)
            proctype = i32(m, o + 164)
            surface = cstr(m, o + i32(m, o + 176)) if i32(m, o + 176) else ""
            self.bones.append({"name": name, "raw": raw, "parent": parent, "proctype": proctype, "surface": surface})
        if any(bone["parent"] >= index for index, bone in enumerate(self.bones)):
            raise ValueError("a bone before its parent")
        if any(bone["proctype"] for bone in self.bones):
            raise ValueError("a part with procedural bones")
        if self.numflexdesc or self.numincludemodels:
            raise ValueError("a part with flexes or included models")
        self.textures = [cstr(m, self.textureindex + t * TEXTURE_SIZE + i32(m, self.textureindex + t * TEXTURE_SIZE)) for t in range(self.numtextures)]
        self.cds = [cstr(m, i32(m, self.cdtextureindex + c * 4)) for c in range(self.numcdtextures)]
        self.skins = [struct.unpack_from("<h", m, self.skinindex + 2 * k)[0] for k in range(self.numskinref * self.numskinfamilies)]
        self.lod0 = self.vvd_lod0()

    def vvd_lod0(self):
        v = self.v
        numfix, fixstart, vstart, tstart = i32(v, 48), i32(v, 52), i32(v, 56), i32(v, 60)
        count = i32(v, 16)
        verts = [v[vstart + k * VERTEX_SIZE: vstart + (k + 1) * VERTEX_SIZE] for k in range((tstart - vstart) // VERTEX_SIZE)]
        tans = [v[tstart + k * TANGENT_SIZE: tstart + (k + 1) * TANGENT_SIZE] for k in range(len(verts))]
        if numfix == 0:
            return verts[:count], tans[:count]
        out_v, out_t = [], []
        for k in range(numfix):
            lod, source, n = struct.unpack_from("<iii", v, fixstart + 12 * k)
            if lod >= 0:
                out_v += verts[source:source + n]
                out_t += tans[source:source + n]
        return out_v, out_t

    def material_path(self, texture):
        override = self.spec.get("material")
        if override:
            return override
        resolved = self.spec.get("materials")
        if resolved and texture < len(resolved):
            return resolved[texture]
        cd = self.cds[0] if self.cds else ""
        return (cd + self.textures[texture]).replace("/", "\\")

    def chosen_meshes(self):
        """Yields (model_name, mesh_raw, mesh vertices, mesh tangents, texture index, vtx mesh offset) for the chosen submodels."""
        m, x = self.m, self.x
        groups = self.spec.get("bodygroups") or {}
        skin = self.spec.get("skin") or 0
        if skin >= self.numskinfamilies:
            skin = 0
        verts, tans = self.lod0
        vtx_bodyparts = i32(x, 32)
        for b in range(self.numbodyparts):
            bp = self.bodypartindex + b * 16
            name = cstr(m, bp + i32(m, bp))
            nummodels, modelindex = i32(m, bp + 4), i32(m, bp + 12)
            choice = groups.get(name, 0)
            if not (0 <= choice < nummodels):
                choice = 0
            mo = bp + modelindex + choice * MODEL_SIZE
            nummeshes, meshindex, mverts, vertexindex = i32(m, mo + 72), i32(m, mo + 76), i32(m, mo + 80), i32(m, mo + 84)
            tangentsindex = i32(m, mo + 88)
            base_v = vertexindex // VERTEX_SIZE
            base_t = tangentsindex // TANGENT_SIZE
            xbp = vtx_bodyparts + b * 8
            xmodel = xbp + i32(x, xbp + 4) + choice * 8
            xlod = xmodel + i32(x, xmodel + 4)
            xmeshes = xlod + i32(x, xlod + 4)
            for k in range(nummeshes):
                me = mo + meshindex + k * MESH_SIZE
                material, mverts_k, offset = i32(m, me), i32(m, me + 8), i32(m, me + 12)
                if i32(m, me + 16):
                    raise ValueError("a mesh with flexes")
                texture = self.skins[skin * self.numskinref + material]
                mv = verts[base_v + offset: base_v + offset + mverts_k]
                mt = tans[base_t + offset: base_t + offset + mverts_k]
                if len(mv) != mverts_k:
                    raise ValueError("vertex range out of the VVD")
                yield {"raw": m[me:me + MESH_SIZE], "verts": mv, "tans": mt, "texture": texture, "vtx": xmeshes + k * 9}


class Writer:
    def __init__(self):
        self.data = bytearray()
        self.strings = []

    def tell(self):
        return len(self.data)

    def add(self, raw):
        at = len(self.data)
        self.data += raw
        return at

    def align(self, n=4):
        while len(self.data) % n:
            self.data += b"\0"

    def put_i32(self, offset, value):
        struct.pack_into("<i", self.data, offset, value)

    def string(self, owner, field, text):
        self.strings.append((owner, field, text))

    def write_strings(self):
        table = {}
        for owner, field, text in self.strings:
            if text not in table:
                table[text] = self.add(text.encode("latin1") + b"\0")
            self.put_i32(field, table[text] - owner)


def vtx_mesh(x, offset):
    """The strip groups of one VTX mesh, as (flags, vertices bytes, indices bytes, strips[(header bytes, bone state changes)])."""
    numsg, sgoffset, flags = i32(x, offset), i32(x, offset + 4), x[offset + 8]
    groups = []
    for g in range(numsg):
        sg = offset + sgoffset + g * 25
        nv, vo, ni, io, ns, so, gflags = struct.unpack_from("<iiiiiiB", x, sg)
        verts = x[sg + vo: sg + vo + nv * 9]
        indices = x[sg + io: sg + io + ni * 2]
        strips = []
        for s in range(ns):
            st = sg + so + s * 27
            header = bytearray(x[st:st + 27])
            nbsc, bsco = struct.unpack_from("<ii", x, st + 19)
            changes = [struct.unpack_from("<ii", x, st + bsco + 8 * c) for c in range(nbsc)]
            strips.append((header, changes))
        groups.append((gflags, verts, indices, strips))
    return flags, groups


def rotate(m, v):
    return tuple(m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3))


def bake_vertex(vert, bake_t):
    """Linear blend of a vertex by its baked bones' transforms (identity for live ones), before its bones are remapped."""
    count = vert[15]
    weights = struct.unpack_from("<3f", vert, 0)
    if not any(vert[12 + k] in bake_t for k in range(count)):
        return
    pos = struct.unpack_from("<3f", vert, 16)
    normal = struct.unpack_from("<3f", vert, 28)
    out_p, out_n = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]
    total = 0.0
    for k in range(count):
        m = bake_t.get(vert[12 + k])
        w = weights[k]
        total += w
        if m is None:
            p, n = pos, normal
        else:
            p = tuple(c + m[i][3] for i, c in enumerate(rotate(m, pos)))
            n = rotate(m, normal)
        for i in range(3):
            out_p[i] += w * p[i]
            out_n[i] += w * n[i]
    if total <= 0:
        return
    out_p = [c / total for c in out_p]
    length = sum(c * c for c in out_n) ** 0.5 or 1.0
    struct.pack_into("<3f", vert, 16, *out_p)
    struct.pack_into("<3f", vert, 28, *[c / length for c in out_n])


def transform_vertex(vert, m):
    """Moves a rigid part's vertex (position and normal) by a 3x4 matrix into the body's bind pose."""
    pos = struct.unpack_from("<3f", vert, 16)
    normal = struct.unpack_from("<3f", vert, 28)
    moved = tuple(c + m[i][3] for i, c in enumerate(rotate(m, pos)))
    struct.pack_into("<3f", vert, 16, *moved)
    struct.pack_into("<3f", vert, 28, *rotate(m, normal))


def m_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0) for j in range(4)] for i in range(3)]


def m_inv(m):
    r = [[m[j][i] for j in range(3)] for i in range(3)]
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[i] + [t[i]] for i in range(3)]


def pose_to_bone(raw):
    f = struct.unpack_from("<12f", raw, 96)
    return [list(f[0:4]), list(f[4:8]), list(f[8:12])]


def quat_matrix(q, pos):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), pos[0]],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), pos[1]],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), pos[2]]]


def body_rest(body_structs):
    """Each body bone's rest transform in model space, from the bone structs' parent, pos and quat."""
    by_index = sorted(body_structs.values(), key=lambda b: b["index"])
    rest = {}
    for b in by_index:
        local = quat_matrix(struct.unpack_from("<4f", b["raw"], 44), struct.unpack_from("<3f", b["raw"], 32))
        parent = b["parent"]
        rest[b["index"]] = m_mul(rest[parent], local) if parent >= 0 else local
    return {b["name"].lower(): rest[b["index"]] for b in by_index}


def euler_quat(x, y, z):
    """Source's AngleQuaternion for a RadianEuler."""
    sr, cr = math.sin(x * 0.5), math.cos(x * 0.5)
    sp, cp = math.sin(y * 0.5), math.cos(y * 0.5)
    sy, cy = math.sin(z * 0.5), math.cos(z * 0.5)
    sr_cp, cr_sp = sr * cp, cr * sp
    cr_cp, sr_sp = cr * cp, sr * sp
    return (sr_cp * cy - cr_sp * sy, cr_sp * cy + sr_cp * sy, cr_cp * sy - sr_sp * cy, cr_cp * cy + sr_sp * sy)


def quat64(bits):
    x = ((bits & 0x1FFFFF) - 1048576) / 1048576.5
    y = (((bits >> 21) & 0x1FFFFF) - 1048576) / 1048576.5
    z = (((bits >> 42) & 0x1FFFFF) - 1048576) / 1048576.5
    w = math.sqrt(max(0.0, 1.0 - x * x - y * y - z * z))
    return (x, y, z, -w if bits >> 63 else w)


def quat48(a, b, c):
    x = (a - 32768) / 32768.0
    y = (b - 32768) / 32768.0
    z = ((c & 0x7FFF) - 16384) / 16384.0
    w = math.sqrt(max(0.0, 1.0 - x * x - y * y - z * z))
    return (x, y, z, -w if c & 0x8000 else w)


def first_frame_value(m, valueptr, k):
    """Frame 0 of an animation value track (mstudioanimvalue_t), or 0 when the component has none."""
    offset = struct.unpack_from("<h", m, valueptr + 2 * k)[0]
    if offset == 0:
        return 0.0
    track = valueptr + offset
    valid = m[track]
    return float(struct.unpack_from("<h", m, track + 2)[0]) if valid > 0 else 0.0


def part_locals(part):
    """Each bone's local transform in frame 0 of the part's first animation, which the engine draws a lone part in;
    the bone struct's pos and quat where the animation has no data for it (or is external or a delta)."""
    m = part.m
    locals_ = [quat_matrix(struct.unpack_from("<4f", b["raw"], 44), struct.unpack_from("<3f", b["raw"], 32)) for b in part.bones]
    numanim, animindex = i32(m, 156 + 4 * 6), i32(m, 156 + 4 * 7)
    if numanim < 1 or animindex <= 0 or i32(m, animindex + 52) != 0:
        return locals_
    if i32(m, animindex + 84) != 0:
        ptr = animindex + i32(m, animindex + i32(m, animindex + 80) + 4)
    else:
        ptr = animindex + i32(m, animindex + 56)
    for _ in range(len(part.bones) + 1):
        bone, flags, nxt = struct.unpack_from("<BBh", m, ptr)
        if bone >= len(part.bones):
            break
        raw = part.bones[bone]["raw"]
        quat = struct.unpack_from("<4f", raw, 44)
        pos = struct.unpack_from("<3f", raw, 32)
        data = ptr + 4
        if flags & 0x01:
            quat = quat48(*struct.unpack_from("<HHH", m, data))
            data += 6
        if flags & 0x20:
            quat = quat64(struct.unpack_from("<Q", m, data)[0])
            data += 8
        if flags & 0x02:
            pos = struct.unpack_from("<3e", m, data)
            data += 6
        if flags & 0x04:
            rot = struct.unpack_from("<3f", raw, 60)
            scale = struct.unpack_from("<3f", raw, 84)
            quat = euler_quat(*[rot[k] + first_frame_value(m, data, k) * scale[k] for k in range(3)])
            data += 6
        if flags & 0x08:
            base = struct.unpack_from("<3f", raw, 32)
            scale = struct.unpack_from("<3f", raw, 72)
            pos = tuple(base[k] + first_frame_value(m, data, k) * scale[k] for k in range(3))
        if not flags & 0x10:
            locals_[bone] = quat_matrix(quat, pos)
        if nxt == 0:
            break
        ptr += nxt
    return locals_


def part_rest_of(part):
    """A part's bones' rest transforms in its model space, from the bone structs, by index: what a bonemerged part's own
    bones take (checked against the game's bone matrices, client/mesh_bake_probe)."""
    if getattr(part, "rest_cache", None) is None:
        rest = []
        for bone in part.bones:
            local = quat_matrix(struct.unpack_from("<4f", bone["raw"], 44), struct.unpack_from("<3f", bone["raw"], 32))
            rest.append(m_mul(rest[bone["parent"]], local) if bone["parent"] >= 0 else local)
        part.rest_cache = rest
    return part.rest_cache


def part_pose_of(part):
    """A part's bones' transforms in its model space in frame 0 of its first animation, by index: the pose the engine
    draws a part placed on a bone or attachment in (its own sequence; the surplus sack's root turns 90 degrees there)."""
    if getattr(part, "pose_cache", None) is None:
        locals_ = part_locals(part)
        pose = []
        for index, bone in enumerate(part.bones):
            pose.append(m_mul(pose[bone["parent"]], locals_[index]) if bone["parent"] >= 0 else locals_[index])
        part.pose_cache = pose
    return part.pose_cache


def merge(specs, out_dir, name, bake=None):
    """bake: {"ancestor": {lowercase bone name: live ancestor name}, "body": {lowercase name: raw bone struct}} folds every
    named bone's weights into its live ancestor (exact in the bind pose; no per-character scales applied yet)."""
    parts = [Part(spec) for spec in specs]
    bake = bake or {"ancestor": {}, "body": {}}
    baked_names = set()

    def body_bone(name):
        key = name.lower()
        if key not in bone_index:
            bone_index[key] = len(bones)
            bones.append({"name": name, "raw": bytearray(bake["body"][key]), "parent": -1, "proctype": 0, "surface": "", "merged_parent": -1, "flags": 0})
        bones[bone_index[key]]["flags"] |= 0x400
        return bone_index[key]

    # bones: one merged bone per (name, bind pose). A part bone keeps its own bind; a baked bone becomes its live ancestor
    # with the bind that reproduces it there (R_a->s * P_s). Parts that agree on a bind share the bone; parts that don't
    # get their own bone of the same name, which bonemerge drives from the body's bone just the same. No vertex moves.
    bones, bone_index = [], {}
    remaps = []
    rest = bake.get("rest", {})

    def find_or_add(name, bind, raw, parent):
        key = name.lower()
        for index in bone_index.get(key, []):
            other = pose_to_bone(bones[index]["raw"])
            if max(abs(other[i][j] - bind[i][j]) for i in range(3) for j in range(4)) < 1e-3:
                return index
        raw = bytearray(raw)
        struct.pack_into("<12f", raw, 96, *[c for row in bind for c in row])
        bones.append({"name": name, "raw": raw, "parent": parent, "proctype": 0, "surface": "", "merged_parent": parent, "flags": 0x400})
        bone_index.setdefault(key, []).append(len(bones) - 1)
        return len(bones) - 1

    for part in parts:
        rigid = part.spec.get("rigid")
        if rigid:
            # a part that follows a bone or attachment: every vertex goes onto that one body bone (transform_vertex)
            target = find_or_add(rigid["bone_name"], pose_to_bone(rigid["bone_raw"]), rigid["bone_raw"], -1)
            remaps.append([target] * max(len(part.bones), 1))
            continue
        remap = []
        for index, bone in enumerate(part.bones):
            key = bone["name"].lower()
            own_bind = pose_to_bone(bone["raw"])
            ancestor = bake["ancestor"].get(key)
            if ancestor and ancestor.lower() in bake["body"] and ancestor.lower() in rest:
                # the nearest bone up the part's own chain that the body has (the bone itself when the body has it)
                q = index
                while q >= 0 and part.bones[q]["name"].lower() not in rest:
                    q = part.bones[q]["parent"]
                if q >= 0:
                    part_rest = part_rest_of(part)
                    relative = m_mul(m_mul(m_inv(rest[ancestor.lower()]), rest[part.bones[q]["name"].lower()]), m_mul(m_inv(part_rest[q]), part_rest[index]))
                    baked_names.add(bone["name"])
                    remap.append(find_or_add(ancestor, m_mul(relative, own_bind), bake["body"][ancestor.lower()], -1))
                    continue
            parent = remap[bone["parent"]] if bone["parent"] >= 0 else -1
            target = find_or_add(bone["name"], own_bind, bone["raw"], parent)
            bones[target]["flags"] |= struct.unpack_from("<i", bone["raw"], 160)[0]
            remap.append(target)
        remaps.append(remap)
    if len(bones) > 255:
        raise ValueError("more than 255 bones")
    rebinds = sum(len(v) - 1 for v in bone_index.values())

    # meshes and textures
    textures, texture_index = [], {}
    meshes = []
    for p, part in enumerate(parts):
        for mesh in part.chosen_meshes():
            path = part.material_path(mesh["texture"])
            key = path.lower()
            if key not in texture_index:
                texture_index[key] = len(textures)
                textures.append(path)
            mesh["material"] = texture_index[key]
            mesh["part"] = p
            meshes.append(mesh)

    checksum = struct.unpack("<i", hashlib.sha256(name.encode()).digest()[:4])[0]

    # VVD
    out_verts, out_tans = [], []
    for mesh in meshes:
        remap = remaps[mesh["part"]]
        mesh["offset"] = len(out_verts)
        rigid = parts[mesh["part"]].spec.get("rigid")
        pose_t = {}
        if rigid:
            part_pose = part_pose_of(parts[mesh["part"]])
            pose_t = {i: m_mul(part_pose[i], pose_to_bone(b["raw"])) for i, b in enumerate(parts[mesh["part"]].bones)}
        first_bones = []
        for raw in mesh["verts"]:
            vert = bytearray(raw)
            count = vert[15]
            first_bones.append(vert[12])
            if rigid:
                bake_vertex(vert, pose_t)

            for k in range(3):
                vert[12 + k] = remap[vert[12 + k]] if k < count else 0
            if rigid:
                transform_vertex(vert, rigid["matrix"])
            out_verts.append(bytes(vert))
        if rigid:
            for k, raw in enumerate(mesh["tans"]):
                tan = bytearray(raw)
                x, y, z = struct.unpack_from("<3f", tan, 0)
                own = pose_t.get(first_bones[k]) if k < len(first_bones) else None
                if own is not None:
                    x, y, z = rotate(own, (x, y, z))
                struct.pack_into("<3f", tan, 0, *rotate(rigid["matrix"], (x, y, z)))
                out_tans.append(bytes(tan))
        else:
            out_tans += mesh["tans"]
    total = len(out_verts)
    vvd = bytearray(struct.pack("<4sii i", b"IDSV", 4, checksum, 1))
    vvd += struct.pack("<8i", *([total] * 8))
    vvd += struct.pack("<iiii", 0, 64, 64, 64 + total * VERTEX_SIZE)
    vvd += b"".join(out_verts) + b"".join(out_tans)

    # VTX: one bodypart, one model, one LOD, the meshes' strip groups kept whole with their bone state changes remapped
    vtx = bytearray(36)
    struct.pack_into("<iiHHiiiiii", vtx, 0, 7, 24, 53, 9, 3, checksum, 1, 0, 1, 36)
    bp = len(vtx); vtx += struct.pack("<ii", 1, 8)
    model = len(vtx); vtx += struct.pack("<ii", 1, 8)
    lod = len(vtx); vtx += struct.pack("<iif", len(meshes), 12, 0.0)
    mesh_headers = len(vtx); vtx += bytes(9 * len(meshes))
    parsed = [vtx_mesh(parts[mesh["part"]].x, mesh["vtx"]) for mesh in meshes]
    group_headers = []
    for k, (flags, groups) in enumerate(parsed):
        at = len(vtx)
        struct.pack_into("<iiB", vtx, mesh_headers + 9 * k, len(groups), at - (mesh_headers + 9 * k), flags)
        vtx += bytes(25 * len(groups))
        group_headers.append(at)
    for k, (flags, groups) in enumerate(parsed):
        remap = remaps[meshes[k]["part"]]
        for g, (gflags, verts, indices, strips) in enumerate(groups):
            sg = group_headers[k] + 25 * g
            vo = len(vtx) - sg; vtx += verts
            io = len(vtx) - sg; vtx += indices
            so = len(vtx) - sg
            strip_at = len(vtx)
            vtx += bytes(27 * len(strips))
            for s, (header, changes) in enumerate(strips):
                st = strip_at + 27 * s
                bsco = len(vtx) - st
                for hardware, bone in changes:
                    vtx += struct.pack("<ii", hardware, remap[bone])
                header = bytearray(header)
                struct.pack_into("<ii", header, 19, len(changes), bsco)
                vtx[st:st + 27] = header
            struct.pack_into("<iiiiiiB", vtx, sg, len(verts) // 9, vo, len(indices) // 2, io, len(strips), so, gflags)
    matrep = len(vtx)
    vtx += struct.pack("<ii", 0, 0)
    struct.pack_into("<i", vtx, 24, matrep)

    # MDL
    w = Writer()
    w.add(bytes(HEADER_SIZE))
    h2 = w.add(bytes(HEADER2_SIZE))
    first = parts[0]
    struct.pack_into("<4si", w.data, 0, b"IDST", 48)
    struct.pack_into("<i", w.data, 8, checksum)
    struct.pack_into("<64s", w.data, 12, ("rcx_merged/" + name + ".mdl").encode()[:63])
    vectors = list(first.vectors)
    for part in parts[1:]:
        for k in range(3):
            vectors[6 + k] = min(vectors[6 + k], part.vectors[6 + k])
            vectors[9 + k] = max(vectors[9 + k], part.vectors[9 + k])
            vectors[12 + k] = min(vectors[12 + k], part.vectors[12 + k])
            vectors[15 + k] = max(vectors[15 + k], part.vectors[15 + k])
    struct.pack_into("<18f", w.data, 80, *vectors)
    struct.pack_into("<i", w.data, 152, first.flags)
    header = {}

    w.align()
    header["boneindex"] = w.tell()
    for b, bone in enumerate(bones):
        at = w.add(bytes(bone["raw"]))
        w.string(at, at + 0, bone["name"])
        struct.pack_into("<i", w.data, at + 4, bone["merged_parent"])
        struct.pack_into("<i", w.data, at + 160, bone["flags"])
        struct.pack_into("<6i", w.data, at + 8, *([-1] * 6))
        struct.pack_into("<ii", w.data, at + 164, 0, 0)
        w.string(at, at + 176, bone["surface"])

    header["hitboxsetindex"] = w.tell()
    at = w.add(struct.pack("<iii", 0, 0, 12))
    w.string(at, at, "default")

    header["bonetablebynameindex"] = w.tell()
    order = sorted(range(len(bones)), key=lambda k: bones[k]["name"].lower())
    w.add(bytes(order))
    w.align()

    # one animation with no bone data: every bone takes its bind pose, which bonemerge overrides
    anim_at = w.add(bytes(100))
    header["localanimindex"] = anim_at
    struct.pack_into("<i", w.data, anim_at, -anim_at)
    w.string(anim_at, anim_at + 4, "@idle")
    struct.pack_into("<f", w.data, anim_at + 8, 30.0)
    struct.pack_into("<ii", w.data, anim_at + 12, 0, 1)
    empty = w.add(struct.pack("<BBh", 255, 0, 0))
    struct.pack_into("<i", w.data, anim_at + 56, empty - anim_at)
    w.align()

    seq_at = w.add(bytes(212))
    header["localseqindex"] = seq_at
    struct.pack_into("<i", w.data, seq_at, -seq_at)
    w.string(seq_at, seq_at + 4, "idle")
    w.string(seq_at, seq_at + 8, "")
    struct.pack_into("<iii", w.data, seq_at + 12, 0, -1, 0)
    struct.pack_into("<6f", w.data, seq_at + 32, *vectors[6:12])
    struct.pack_into("<i", w.data, seq_at + 56, 1)
    animidx = w.add(struct.pack("<hh", 0, 0))
    struct.pack_into("<i", w.data, seq_at + 60, animidx - seq_at)
    struct.pack_into("<ii", w.data, seq_at + 68, 1, 1)
    struct.pack_into("<ii", w.data, seq_at + 76, -1, -1)
    struct.pack_into("<i", w.data, seq_at + 100, 0)
    struct.pack_into("<ff", w.data, seq_at + 104, 0.2, 0.2)
    weights = w.add(struct.pack("<%df" % len(bones), *([1.0] * len(bones))))
    struct.pack_into("<i", w.data, seq_at + 156, weights - seq_at)
    struct.pack_into("<i", w.data, seq_at + 180, 0)

    w.align()
    header["bodypartindex"] = w.tell()
    bp_at = w.add(struct.pack("<iiii", 0, 1, 1, 16))
    w.string(bp_at, bp_at, "merged")
    model_at = w.add(bytes(MODEL_SIZE))
    struct.pack_into("<64s", w.data, model_at, b"merged")
    radius = max(abs(c) for c in vectors[6:12]) * 1.8
    struct.pack_into("<if", w.data, model_at + 64, 0, radius)
    meshes_at = w.tell()
    struct.pack_into("<iiiii", w.data, model_at + 72, len(meshes), meshes_at - model_at, total, 0, 0)
    for k, mesh in enumerate(meshes):
        at = w.add(bytes(mesh["raw"]))
        n = len(mesh["verts"])
        struct.pack_into("<iiii", w.data, at, mesh["material"], model_at - at, n, mesh["offset"])
        struct.pack_into("<ii", w.data, at + 16, 0, 0)
        struct.pack_into("<i", w.data, at + 32, k)
        struct.pack_into("<i", w.data, at + 48, 0)
        struct.pack_into("<8i", w.data, at + 52, *([n] * 8))

    w.align()
    header["textureindex"] = w.tell()
    for path in textures:
        at = w.add(bytes(TEXTURE_SIZE))
        w.string(at, at, path)
    header["cdtextureindex"] = w.tell()
    cd_at = w.add(struct.pack("<i", 0))
    header["skinindex"] = w.tell()
    w.add(struct.pack("<%dh" % len(textures), *range(len(textures))))
    w.align()

    # empty lists point at the end of the header area, as studiomdl does
    blank = w.tell()
    header["keyvalueindex"] = blank
    for key in ("bonecontrollerindex", "localattachmentindex", "localnodeindex", "localnodenameindex", "flexdescindex",
                "flexcontrollerindex", "flexruleindex", "ikchainindex", "mouthindex", "localposeparamindex",
                "localikautoplaylockindex", "includemodelindex", "animblockindex", "flexcontrolleruiindex"):
        header[key] = blank

    fields = ["numbones", "boneindex", "numbonecontrollers", "bonecontrollerindex", "numhitboxsets", "hitboxsetindex",
              "numlocalanim", "localanimindex", "numlocalseq", "localseqindex", "activitylistversion", "eventsindexed",
              "numtextures", "textureindex", "numcdtextures", "cdtextureindex", "numskinref", "numskinfamilies", "skinindex",
              "numbodyparts", "bodypartindex", "numlocalattachments", "localattachmentindex", "numlocalnodes", "localnodeindex",
              "localnodenameindex", "numflexdesc", "flexdescindex", "numflexcontrollers", "flexcontrollerindex", "numflexrules",
              "flexruleindex", "numikchains", "ikchainindex", "nummouths", "mouthindex", "numlocalposeparameters",
              "localposeparamindex", "surfacepropindex", "keyvalueindex", "keyvaluesize", "numlocalikautoplaylocks",
              "localikautoplaylockindex", "mass", "contents", "numincludemodels", "includemodelindex", "virtualModel",
              "szanimblocknameindex", "numanimblocks", "animblockindex", "animblockModel", "bonetablebynameindex"]
    header.update({"numbones": len(bones), "numhitboxsets": 1, "numlocalanim": 1, "numlocalseq": 1, "numtextures": len(textures),
                   "numcdtextures": 1, "numskinref": len(textures), "numskinfamilies": 1, "numbodyparts": 1, "contents": 1})
    for k, field in enumerate(fields):
        value = header.get(field, 0)
        if field == "mass":
            struct.pack_into("<f", w.data, 156 + 4 * k, 1.0)
        else:
            struct.pack_into("<i", w.data, 156 + 4 * k, value)
    struct.pack_into("<i", w.data, 400, h2)

    # strings, referenced from the header and studiohdr2 relative to their owners
    w.string(0, 156 + 4 * fields.index("surfacepropindex"), first.bones[0]["surface"] if first.bones else "")
    w.string(0, 156 + 4 * fields.index("szanimblocknameindex"), "")
    w.string(h2, h2 + 20, "rcx_merged/" + name + ".mdl")
    w.string(0, cd_at, "")
    w.write_strings()
    w.align()
    struct.pack_into("<i", w.data, 76, len(w.data))
    struct.pack_into("<f", w.data, h2 + 12, 0.0)

    os.makedirs(out_dir, exist_ok=True)
    outputs = {"mdl": bytes(w.data), "vvd": bytes(vvd), "dx90.vtx": bytes(vtx)}
    for ext, data in outputs.items():
        path = os.path.join(out_dir, name + "." + ext)
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    return {"bones": len(bones), "unbakedParts": sum(1 for part in parts if getattr(part, "bake_skipped", False)), "rebinds": rebinds, "baked": sorted(baked_names), "textures": textures, "meshes": len(meshes), "vertices": total,
            "bytes": {ext: len(data) for ext, data in outputs.items()}}
