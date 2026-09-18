"""Read Source BSP geometry for the replay viewer."""

import argparse
import json
import lzma
import os
import struct
import sys

HEADER_LUMPS = 64

LUMP_ENTITIES = 0
LUMP_PLANES = 1
LUMP_TEXDATA = 2
LUMP_VERTEXES = 3
LUMP_TEXINFO = 6
LUMP_FACES = 7
LUMP_EDGES = 12
LUMP_SURFEDGES = 13
LUMP_MODELS = 14
LUMP_DISPINFO = 26
LUMP_DISP_VERTS = 33
LUMP_TEXDATA_STRING_DATA = 43
LUMP_TEXDATA_STRING_TABLE = 44

# texinfo flags
SURF_SKY = 0x4
SURF_NODRAW = 0x80
SURF_SKIP = 0x200
SURF_TRIGGER = 0x40
SURF_HINT = 0x100

LZMA_ID = 0x414D5A4C  # 'LZMA' little-endian

class Lump(object):
    __slots__ = ("offset", "length", "version", "fourcc")

    def __init__(self, offset, length, version, fourcc):
        self.offset = offset
        self.length = length
        self.version = version
        self.fourcc = fourcc

def decompress_lump(blob):
    """Source's compressed-lump container. Layout:"""
    if len(blob) < 17:
        return blob
    ident, actual_size, lzma_size = struct.unpack_from("<III", blob, 0)
    if ident != LZMA_ID:
        return blob
    props = blob[12:17]
    payload = blob[17:17 + lzma_size]
    alone = props + struct.pack("<Q", actual_size) + payload
    try:
        out = lzma.decompress(alone, format=lzma.FORMAT_ALONE)
    except lzma.LZMAError:
        # Some lumps omit the end marker and raise on a clean finish; decompress
        # incrementally and keep what was read.
        d = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
        out = d.decompress(alone)
    return out[:actual_size]

class BSP(object):
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()

        ident, self.version = struct.unpack_from("<4si", self.data, 0)
        if ident != b"VBSP":
            raise ValueError("not a VBSP file: %r" % ident)

        self.lumps = []
        for i in range(HEADER_LUMPS):
            off, ln, ver, fourcc = struct.unpack_from("<iii4s", self.data, 8 + i * 16)
            self.lumps.append(Lump(off, ln, ver, fourcc))

        self._cache = {}

    def lump(self, index):
        if index in self._cache:
            return self._cache[index]
        l = self.lumps[index]
        raw = self.data[l.offset:l.offset + l.length]
        if len(raw) >= 4 and struct.unpack_from("<I", raw, 0)[0] == LZMA_ID:
            raw = decompress_lump(raw)
        self._cache[index] = raw
        return raw

    # ---------------------------------------------------------------- lumps --

    def vertexes(self):
        b = self.lump(LUMP_VERTEXES)
        n = len(b) // 12
        return [struct.unpack_from("<3f", b, i * 12) for i in range(n)]

    def edges(self):
        b = self.lump(LUMP_EDGES)
        n = len(b) // 4
        return [struct.unpack_from("<2H", b, i * 4) for i in range(n)]

    def surfedges(self):
        b = self.lump(LUMP_SURFEDGES)
        n = len(b) // 4
        return list(struct.unpack_from("<%di" % n, b, 0))

    def planes(self):
        b = self.lump(LUMP_PLANES)
        n = len(b) // 20
        return [struct.unpack_from("<3ffi", b, i * 20) for i in range(n)]

    def texinfo(self):
        """(textureVecs[2][4], lightmapVecs[2][4], flags, texdata) = 72 bytes."""
        b = self.lump(LUMP_TEXINFO)
        n = len(b) // 72
        out = []
        for i in range(n):
            o = i * 72
            flags, texdata = struct.unpack_from("<ii", b, o + 64)
            out.append((flags, texdata))
        return out

    def texdata(self):
        """(reflectivity[3], nameStringTableID, w,h, view_w,view_h) = 32 bytes."""
        b = self.lump(LUMP_TEXDATA)
        n = len(b) // 32
        return [struct.unpack_from("<i", b, i * 32 + 12)[0] for i in range(n)]

    def texture_names(self):
        table = self.lump(LUMP_TEXDATA_STRING_TABLE)
        data = self.lump(LUMP_TEXDATA_STRING_DATA)
        n = len(table) // 4
        offs = struct.unpack_from("<%di" % n, table, 0)
        names = []
        for o in offs:
            end = data.find(b"\0", o)
            names.append(data[o:end].decode("ascii", "replace"))
        return names

    def faces(self):
        """
        dface_t is 56 bytes in v19/v20. We need planenum, side, firstedge,
        numedges, texinfo, dispinfo.
        """
        b = self.lump(LUMP_FACES)
        n = len(b) // 56
        out = []
        for i in range(n):
            o = i * 56
            planenum, side, onnode = struct.unpack_from("<HBB", b, o)
            firstedge, numedges, texinfo_i, dispinfo = struct.unpack_from("<ihhh", b, o + 4)
            out.append({
                "planenum": planenum, "side": side,
                "firstedge": firstedge, "numedges": numedges,
                "texinfo": texinfo_i, "dispinfo": dispinfo,
            })
        return out

    def dispinfo(self):
        """ddispinfo_t is 176 bytes in v20."""
        b = self.lump(LUMP_DISPINFO)
        n = len(b) // 176
        out = []
        for i in range(n):
            o = i * 176
            sx, sy, sz = struct.unpack_from("<3f", b, o)
            disp_vert_start, disp_tri_start, power = struct.unpack_from("<iii", b, o + 12)
            map_face = struct.unpack_from("<H", b, o + 28)[0]
            out.append({
                "start": (sx, sy, sz),
                "disp_vert_start": disp_vert_start,
                "power": power,
                "map_face": map_face,
            })
        return out

    def disp_verts(self):
        """dDispVert is 20 bytes: vec (12), dist (4), alpha (4)."""
        b = self.lump(LUMP_DISP_VERTS)
        n = len(b) // 20
        out = []
        for i in range(n):
            o = i * 20
            vx, vy, vz, dist = struct.unpack_from("<4f", b, o)
            out.append((vx, vy, vz, dist))
        return out

    def models(self):
        """dmodel_t is 48 bytes; model 0 is the world."""
        b = self.lump(LUMP_MODELS)
        n = len(b) // 48
        out = []
        for i in range(n):
            o = i * 48
            firstface, numfaces = struct.unpack_from("<ii", b, o + 40)
            out.append({"firstface": firstface, "numfaces": numfaces})
        return out

    def entities(self):
        return self.lump(LUMP_ENTITIES).decode("ascii", "replace")

def build_world_mesh(bsp, skip_tools=True):
    """Triangulate the world model's faces."""
    verts = bsp.vertexes()
    edges = bsp.edges()
    surfedges = bsp.surfedges()
    planes = bsp.planes()
    faces = bsp.faces()
    tinfo = bsp.texinfo()
    tdata = bsp.texdata()
    tnames = bsp.texture_names()
    dinfos = bsp.dispinfo()
    dverts = bsp.disp_verts()
    models = bsp.models()

    world = models[0]
    positions = []
    normals = []
    indices = []
    stats = {"faces": 0, "disp": 0, "skipped": 0, "tris": 0}

    def face_winding(f):
        """Resolve a face's vertex ring through the signed surfedge table."""
        ring = []
        for k in range(f["numedges"]):
            se = surfedges[f["firstedge"] + k]
            if se >= 0:
                ring.append(edges[se][0])
            else:
                ring.append(edges[-se][1])
        return [verts[i] for i in ring]

    def add_tri(a, b, c, nrm):
        base = len(positions) // 3
        for p in (a, b, c):
            positions.extend(p)
            normals.extend(nrm)
        indices.extend((base, base + 1, base + 2))
        stats["tris"] += 1

    for fi in range(world["firstface"], world["firstface"] + world["numfaces"]):
        f = faces[fi]
        ti = f["texinfo"]
        flags = tinfo[ti][0] if 0 <= ti < len(tinfo) else 0

        if skip_tools and (flags & (SURF_SKY | SURF_NODRAW | SURF_SKIP | SURF_HINT | SURF_TRIGGER)):
            stats["skipped"] += 1
            continue

        nx, ny, nz, _d = planes[f["planenum"]][0], planes[f["planenum"]][1], \
                          planes[f["planenum"]][2], planes[f["planenum"]][3]
        nrm = (nx, ny, nz) if f["side"] == 0 else (-nx, -ny, -nz)

        ring = face_winding(f)

        if f["dispinfo"] >= 0:
            di = dinfos[f["dispinfo"]]
            tess_displacement(ring, di, dverts, add_tri)
            stats["disp"] += 1
        else:
            if len(ring) < 3:
                continue
            for k in range(1, len(ring) - 1):
                add_tri(ring[0], ring[k], ring[k + 1], nrm)
            stats["faces"] += 1

    return positions, normals, indices, stats

def tess_displacement(ring, di, dverts, add_tri):
    """Tessellate one displacement."""
    if len(ring) != 4:
        return
    power = di["power"]
    size = (1 << power) + 1

    # rotate the ring so it begins at the corner nearest `start`
    sx, sy, sz = di["start"]
    best, bestd = 0, None
    for i, p in enumerate(ring):
        d = (p[0] - sx) ** 2 + (p[1] - sy) ** 2 + (p[2] - sz) ** 2
        if bestd is None or d < bestd:
            bestd, best = d, i
    c = [ring[(best + i) % 4] for i in range(4)]

    base = di["disp_vert_start"]
    grid = []
    for i in range(size):
        ti = i / float(size - 1)
        # edges c0->c1 and c3->c2, then interpolate across
        left = [c[0][k] + (c[1][k] - c[0][k]) * ti for k in range(3)]
        right = [c[3][k] + (c[2][k] - c[3][k]) * ti for k in range(3)]
        row = []
        for j in range(size):
            tj = j / float(size - 1)
            p = [left[k] + (right[k] - left[k]) * tj for k in range(3)]
            dv = dverts[base + i * size + j]
            row.append((p[0] + dv[0] * dv[3],
                        p[1] + dv[1] * dv[3],
                        p[2] + dv[2] * dv[3]))
        grid.append(row)

    for i in range(size - 1):
        for j in range(size - 1):
            a, b = grid[i][j], grid[i][j + 1]
            cc, d = grid[i + 1][j + 1], grid[i + 1][j]
            n1 = tri_normal(a, b, cc)
            add_tri(a, b, cc, n1)
            n2 = tri_normal(a, cc, d)
            add_tri(a, cc, d, n2)

def tri_normal(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    l = (nx * nx + ny * ny + nz * nz) ** 0.5
    if l < 1e-9:
        return (0.0, 0.0, 1.0)
    return (nx / l, ny / l, nz / l)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bsp")
    ap.add_argument("--out", help="write a compact binary mesh here")
    ap.add_argument("--info", action="store_true")
    args = ap.parse_args()

    bsp = BSP(args.bsp)
    print("VBSP version %d, %.1f MB" % (bsp.version, os.path.getsize(args.bsp) / 1048576.0))

    if args.info:
        names = bsp.texture_names()
        print("vertexes  : %d" % len(bsp.vertexes()))
        print("faces     : %d" % len(bsp.faces()))
        print("dispinfo  : %d" % len(bsp.dispinfo()))
        print("disp verts: %d" % len(bsp.disp_verts()))
        print("textures  : %d  e.g. %s" % (len(names), ", ".join(names[:4])))

    pos, nrm, idx, stats = build_world_mesh(bsp)
    print("mesh: %d tris  (%d brush faces, %d displacements, %d tool faces skipped)"
          % (stats["tris"], stats["faces"], stats["disp"], stats["skipped"]))

    if args.out:
        write_mesh(args.out, pos, nrm, idx)
        print("wrote %s (%.1f MB)" % (args.out, os.path.getsize(args.out) / 1048576.0))
    return 0

def write_mesh(path, positions, normals, indices):
    """Compact binary: magic, counts, then float32 positions, int8 normals, uint32"""
    import array
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    nverts = len(positions) // 3
    with open(path, "wb") as fh:
        fh.write(b"CSAIMESH")
        fh.write(struct.pack("<II", nverts, len(indices)))
        array.array("f", positions).tofile(fh)
        q = array.array("b", [max(-127, min(127, int(round(v * 127)))) for v in normals])
        q.tofile(fh)
        array.array("I", indices).tofile(fh)

if __name__ == "__main__":
    sys.exit(main())
