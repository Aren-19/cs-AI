"""Read Valve VPK archives."""

import os
import struct

VPK_SIGNATURE = 0x55AA1234
ARCHIVE_IN_DIR = 0x7FFF

class VPKEntry(object):
    __slots__ = ("crc", "preload", "archive_index", "offset", "length")

    def __init__(self, crc, preload, archive_index, offset, length):
        self.crc = crc
        self.preload = preload
        self.archive_index = archive_index
        self.offset = offset
        self.length = length

    @property
    def size(self):
        return len(self.preload) + self.length

class VPK(object):
    def __init__(self, dir_path):
        self.dir_path = dir_path
        self.base = dir_path[:-8] if dir_path.lower().endswith("_dir.vpk") else dir_path[:-4]
        self.entries = {}
        self._archives = {}
        self._parse()

    def _parse(self):
        with open(self.dir_path, "rb") as fh:
            blob = fh.read()

        sig, version = struct.unpack_from("<II", blob, 0)
        if sig != VPK_SIGNATURE:
            raise ValueError("not a VPK: %s" % self.dir_path)

        if version == 1:
            tree_size = struct.unpack_from("<I", blob, 8)[0]
            header = 12
        elif version == 2:
            tree_size = struct.unpack_from("<I", blob, 8)[0]
            header = 28
        else:
            raise ValueError("unsupported VPK version %d" % version)

        self.data_offset = header + tree_size
        p = header
        end = header + tree_size

        def read_cstr():
            nonlocal p
            e = blob.index(b"\0", p)
            s = blob[p:e].decode("ascii", "replace")
            p = e + 1
            return s

        while p < end:
            ext = read_cstr()
            if ext == "":
                break
            while True:
                path = read_cstr()
                if path == "":
                    break
                while True:
                    name = read_cstr()
                    if name == "":
                        break
                    crc, preload_len, archive_index, offset, length, term = \
                        struct.unpack_from("<IHHIIH", blob, p)
                    p += 18
                    preload = b""
                    if preload_len:
                        preload = blob[p:p + preload_len]
                        p += preload_len

                    if path == " ":
                        full = "%s.%s" % (name, ext)
                    else:
                        full = "%s/%s.%s" % (path, name, ext)
                    self.entries[full.lower()] = VPKEntry(
                        crc, preload, archive_index, offset, length)

    def _archive(self, index):
        if index in self._archives:
            return self._archives[index]
        path = self.dir_path if index == ARCHIVE_IN_DIR else "%s_%03d.vpk" % (self.base, index)
        fh = open(path, "rb")
        self._archives[index] = fh
        return fh

    def has(self, path):
        return path.replace("\\", "/").lower() in self.entries

    def read(self, path):
        e = self.entries.get(path.replace("\\", "/").lower())
        if e is None:
            return None
        if e.length == 0:
            return e.preload
        fh = self._archive(e.archive_index)
        base = self.data_offset if e.archive_index == ARCHIVE_IN_DIR else 0
        fh.seek(base + e.offset)
        return e.preload + fh.read(e.length)

    def close(self):
        for fh in self._archives.values():
            try:
                fh.close()
            except OSError:
                pass
        self._archives.clear()

class SourceFS(object):
    """Resolves a Source path the way CS:S does, in the order of its gameinfo.txt:
    custom content, the CS:S packs, the Half-Life 2 packs, loose files, and last
    the content downloaded from servers (where custom maps keep their materials)."""

    HL2_PACKS = ("hl2_textures", "hl2_sound_vo_english", "hl2_sound_misc", "hl2_misc")

    def __init__(self, game_dir, extra_dirs=()):
        install = os.path.dirname(os.path.abspath(game_dir))
        self.sources = []       # ("dir", path) or ("vpk", VPK), searched in order

        custom = os.path.join(game_dir, "custom")
        if os.path.isdir(custom):
            for name in sorted(os.listdir(custom)):
                sub = os.path.join(custom, name)
                if os.path.isdir(sub):
                    self._dir(sub)
                elif name.lower().endswith("_dir.vpk"):
                    self._vpk(sub)
        for name in sorted(os.listdir(game_dir)):
            if name.lower().endswith("_dir.vpk"):
                self._vpk(os.path.join(game_dir, name))
        for name in self.HL2_PACKS:
            self._vpk(os.path.join(install, "hl2", name + "_dir.vpk"))
        self._vpk(os.path.join(install, "platform", "platform_misc_dir.vpk"))
        self._dir(game_dir)
        for d in extra_dirs:
            self._dir(d)
        self._dir(os.path.join(install, "hl2"))
        self._dir(os.path.join(install, "platform"))
        self._dir(os.path.join(game_dir, "download"))

    def _dir(self, path):
        if os.path.isdir(path):
            self.sources.append(("dir", path))

    def _vpk(self, path):
        if os.path.isfile(path):
            try:
                self.sources.append(("vpk", VPK(path)))
            except (ValueError, OSError):
                pass

    @property
    def roots(self):
        return [p for kind, p in self.sources if kind == "dir"]

    @property
    def vpks(self):
        return [v for kind, v in self.sources if kind == "vpk"]

    def read(self, path):
        rel = path.replace("\\", "/").lstrip("/")
        if ".." in rel.split("/"):
            return None
        # os.path.join discards the root when the second part is absolute, so on
        # Windows "C:/anything" walks straight out of the game directory.
        if os.path.isabs(rel) or os.path.splitdrive(rel)[0]:
            return None
        for kind, src in self.sources:
            if kind == "vpk":
                data = src.read(rel)
                if data is not None:
                    return data
                continue
            full = os.path.join(src, rel.replace("/", os.sep))
            # The resolved path must still be under the root.
            try:
                if os.path.commonpath([os.path.realpath(full),
                                       os.path.realpath(src)]) != os.path.realpath(src):
                    continue
            except ValueError:
                continue
            if os.path.isfile(full):
                try:
                    with open(full, "rb") as fh:
                        return fh.read()
                except OSError:
                    pass
        return None

    def stats(self):
        return {"roots": len(self.roots), "vpks": len(self.vpks),
                "entries": sum(len(v.entries) for v in self.vpks)}

if __name__ == "__main__":
    import sys
    from game import CSTRIKE
    game = sys.argv[1] if len(sys.argv) > 1 else CSTRIKE
    fs = SourceFS(game)
    print("mounted:", fs.stats())
    for probe in ("materials/brick/brickwall001a.vmt",
                  "materials/tools/toolsnodraw.vtf",
                  "models/player/ct_urban.mdl"):
        d = fs.read(probe)
        print("  %-42s %s" % (probe, ("%d bytes" % len(d)) if d else "MISSING"))
