"""Finds a model's files the way the game would: the mount.cfg folders on P: first, then the workshop GMAs."""
import os, struct
MOUNTS = r"P:\NextcloudBizz\ResurgenceShared\Workshop"
WORKSHOP = r"C:\Program Files (x86)\Steam\steamapps\workshop\content\4000"

def from_mounts(path):
    for mount in os.listdir(MOUNTS):
        base = os.path.join(MOUNTS, mount)
        full = os.path.join(base, *path.split("/"))
        folder, name = os.path.split(full)
        if os.path.isdir(folder):
            for entry in os.listdir(folder):
                if entry.lower() == name.lower():
                    return open(os.path.join(folder, entry), "rb").read()
        # case-insensitive walk
        cur = base
        ok = True
        for piece in path.split("/"):
            if not os.path.isdir(cur):
                ok = False; break
            match = [e for e in os.listdir(cur) if e.lower() == piece.lower()]
            if not match:
                ok = False; break
            cur = os.path.join(cur, match[0])
        if ok and os.path.isfile(cur):
            return open(cur, "rb").read()
    return None

def from_gmas(path):
    want = path.lower()
    for d in os.listdir(WORKSHOP):
        dd = os.path.join(WORKSHOP, d)
        if not os.path.isdir(dd): continue
        for fn in os.listdir(dd):
            if not fn.endswith(".gma"): continue
            f = open(os.path.join(dd, fn), "rb"); b = f.read(1 << 20)
            if b[:4] != b"GMAD": continue
            p = 21
            while b[p] != 0: p = b.index(b"\0", p) + 1
            p += 1
            for _ in range(3): p = b.index(b"\0", p) + 1
            p += 4
            data_offset = None; offset = 0; found = None
            while True:
                n = struct.unpack_from("<I", b, p)[0]; p += 4
                if n == 0: break
                e = b.index(b"\0", p); name = b[p:e].decode("latin1"); p = e + 1
                size = struct.unpack_from("<q", b, p)[0]; p += 12
                if name.lower() == want: found = (offset, size)
                offset += size
            if found:
                f.seek(p + found[0]); return f.read(found[1])
    return None

def model_files(model):
    base = model[:-4]
    out = {}
    for ext, key in (("mdl", "mdl"), ("vvd", "vvd"), ("dx90.vtx", "vtx")):
        data = from_mounts(base + "." + ext) or from_gmas(base + "." + ext)
        if data is None:
            raise FileNotFoundError(base + "." + ext)
        out[key] = data
    return out
