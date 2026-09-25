"""Local HTTP server for the replay viewer."""

import argparse
import bz2
import json
import os
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpk import SourceFS
from replaystats import stats as replay_stats

from game import CSTRIKE
REPLAY_DIRS = [
    os.path.join(CSTRIKE, r"addons\sourcemod\data\csai\replays"),
    os.path.join(CSTRIKE, r"addons\sourcemod\data\replaybot\0"),
    os.path.join(CSTRIKE, r"addons\sourcemod\data\replaybot\7"),
]
MAPS_DIRS = (os.path.join(CSTRIKE, "maps"), os.path.join(CSTRIKE, "download", "maps"))
CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "webcache")

# The viewer is served from another port, so it is cross-origin and does need a
# CORS header. It does not need "*": that let any page the browser had open read
# whatever this server would return.
ALLOWED_ORIGINS = ("http://127.0.0.1:3000", "http://localhost:3000")

def cors_origin(req):
    origin = req.headers.get("Origin")
    if origin in ALLOWED_ORIGINS:
        return origin
    return ALLOWED_ORIGINS[0]

CSPAK_MISSING = 0xFFFFFFFF

_fs = None
_fs_lock = threading.Lock()
_bz2_locks = {}
_bz2_locks_guard = threading.Lock()

def fs():
    global _fs
    with _fs_lock:
        if _fs is None:
            t0 = time.time()
            _fs = SourceFS(CSTRIKE)
            s = _fs.stats()
            print("[csspak] mounted %d vpk(s), %d entries, %d loose root(s) in %.1fs"
                  % (s["vpks"], s["entries"], s["roots"], time.time() - t0))
        return _fs

def find_replay(name):
    """Resolve a replay name inside REPLAY_DIRS. Never outside them."""
    # This used to accept an absolute path and serve it. Together with a
    # wildcard CORS header that let any page in the browser read any file on
    # the machine while the viewer was running.
    cand = name if name.lower().endswith(".replay") else name + ".replay"
    cand = os.path.basename(cand)
    if not cand or cand in (".", ".."):
        return None
    for d in REPLAY_DIRS:
        p = os.path.join(d, cand)
        if not os.path.isfile(p):
            continue
        try:
            if os.path.commonpath([os.path.realpath(p), os.path.realpath(d)]) != os.path.realpath(d):
                continue
        except ValueError:
            continue
        return p
    return None

def replay_header(path):
    """Read map/time/frames straight out of the shavit header."""
    try:
        with open(path, "rb") as fh:
            blob = fh.read(256)
        nl = blob.index(bytes([10]))
        version = int(blob[:nl].split(b":")[0])
        p = nl + 1
        mapname, style, track, frames, seconds, tickrate = "", 0, 0, 0, 0.0, 100.0
        if version >= 3:
            e = blob.index(bytes([0]), p)
            mapname = blob[p:e].decode("ascii", "replace")
            p = e + 1
            style = blob[p]; p += 1
            track = blob[p]; p += 1
            p += 4                      # preframes
        frames = struct.unpack_from("<i", blob, p)[0]; p += 4
        seconds = struct.unpack_from("<f", blob, p)[0]; p += 4
        if version >= 4:
            p += 4                      # steamid
        if version >= 5:
            p += 4                      # postframes
            tickrate = struct.unpack_from("<f", blob, p)[0]
        return {"map": mapname, "style": style, "track": track,
                "frames": frames, "time": seconds, "tickrate": tickrate}
    except (OSError, ValueError, struct.error, IndexError):
        return None

def list_replays():
    out = []
    for d in REPLAY_DIRS:
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            if n.lower().endswith(".replay"):
                full = os.path.join(d, n)
                info = replay_header(full) or {}
                out.append({"name": n[:-7], "path": full,
                            "size": os.path.getsize(full),
                            "map": info.get("map") or n[:-7],
                            "time": float(info.get("time") or 0.0),
                            "style": int(info.get("style") or 0),
                            "track": int(info.get("track") or 0),
                            "frames": int(info.get("frames") or 0)})
    return out

def bsp_bz2(map_name):
    """Serves <map>.bsp.bz2, compressed once and cached."""
    safe = os.path.basename(map_name)
    src = next((p for p in (os.path.join(d, safe + ".bsp") for d in MAPS_DIRS) if os.path.isfile(p)), None)
    if not src:
        return None

    os.makedirs(CACHE_DIR, exist_ok=True)
    dst = os.path.join(CACHE_DIR, safe + ".bsp.bz2")

    with _bz2_locks_guard:
        lock = _bz2_locks.setdefault(safe, threading.Lock())

    with lock:
        if os.path.isfile(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            return dst
        print("[maps] compressing %s (%.0f MB) - one time, then cached"
              % (safe, os.path.getsize(src) / 1048576.0))
        t0 = time.time()
        tmp = dst + ".tmp"
        comp = bz2.BZ2Compressor(1)      # level 1: the payload barely compresses anyway
        with open(src, "rb") as fin, open(tmp, "wb") as fout:
            while True:
                chunk = fin.read(4 << 20)
                if not chunk:
                    break
                out = comp.compress(chunk)
                if out:
                    fout.write(out)
            fout.write(comp.flush())
        os.replace(tmp, dst)
        print("[maps] %s -> %.0f MB in %.1fs"
              % (safe, os.path.getsize(dst) / 1048576.0, time.time() - t0))
    return dst

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass        # too chatty: a single map load is hundreds of asset requests

    # ------------------------------------------------------------- helpers --

    def _send(self, code, body=b"", ctype="application/octet-stream", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", cors_origin(self))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def _send_json(self, code, obj):
        self._send(code, json.dumps(obj).encode(), "application/json")

    def _send_file(self, path, ctype="application/octet-stream"):
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        self.send_header("Access-Control-Allow-Origin", cors_origin(self))
        self.send_header("Cache-Control", "public, max-age=3600")
        self.end_headers()
        if self.command == "HEAD":
            return
        with open(path, "rb") as fh:
            while True:
                chunk = fh.read(1 << 20)
                if not chunk:
                    break
                self.wfile.write(chunk)

    def do_OPTIONS(self):
        self._send(204, b"", "text/plain", {
            "Access-Control-Allow-Origin": cors_origin(self),
            "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
            "Access-Control-Allow-Headers": "Content-Type",
        })

    # ----------------------------------------------------------------- GET --

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        p = u.path

        if p == "/api/profile":
            return self._send_json(401, {"error": "not signed in"})

        if p == "/api/replay":
            ids = q.get("id") or []
            if not ids:
                return self._send_json(400, {"error": "missing id"})
            path = find_replay(ids[0])
            if path is None:
                return self._send_json(404, {"error": "no such replay", "id": ids[0]})
            return self._send_file(path)

        if p == "/api/replays":
            return self._send_json(200, {"data": list_replays()})

        if p == "/api/times":
            rows = self._times()
            ids = q.get("ids")
            if ids:
                want = set()
                for v in ids:
                    want.update(x for x in v.split(",") if x)
                rows = [r for r in rows if r["_id"] in want]
            if q.get("map"):
                rows = [r for r in rows if r["map"] == q["map"][0]]
            if q.get("has_replay"):
                pass                      # every row here is backed by a replay file

            sort = (q.get("sort", ["Newest"])[0] or "Newest").lower()
            if sort in ("oldest", "old"):
                rows.sort(key=lambda r: r["date"])
            elif sort in ("fastest", "time"):
                rows.sort(key=lambda r: (r["time"] <= 0, r["time"]))
            elif sort == "slowest":
                rows.sort(key=lambda r: (r["time"] <= 0, -r["time"]))
            else:                                     # Newest
                rows.sort(key=lambda r: r["date"], reverse=True)
            for i, r in enumerate(rows):
                r["rank"] = i + 1

            total = len(rows)
            try:
                limit = max(1, min(int(q.get("limit", ["50"])[0]), 500))
                page = max(1, int(q.get("page", ["1"])[0]))
            except ValueError:
                limit, page = 50, 1
            rows = rows[(page - 1) * limit: page * limit]
            return self._send_json(200, {"data": rows, "total": total})

        if p.startswith("/maps/") and p.endswith(".bsp.bz2"):
            name = p[len("/maps/"):-len(".bsp.bz2")]
            path = bsp_bz2(name)
            if path is None:
                return self._send_json(404, {"error": "no such map", "map": name})
            return self._send_file(path)

        if p == "/health":
            return self._send_json(200, {"ok": True, "replays": len(list_replays()),
                                         "fs": fs().stats()})

        return self._send_json(404, {"error": "not found", "path": p})

    def _times(self):
        rows = list_replays()
        # best time per map, so the UI can show a delta like the real site does
        best = {}
        for r in rows:
            t = r["time"]
            if t > 0 and (r["map"] not in best or t < best[r["map"]]):
                best[r["map"]] = t

        out = []
        for i, r in enumerate(rows):
            try:
                st = replay_stats(r["path"])
            except Exception:
                st = {"sync": 0.0, "strafes": 0, "jumps": 0}
            out.append({
                "_id": r["name"],
                "map": r["map"],
                "name": "CsAI",
                "steamid": "0",
                "time": r["time"],
                "date": int(os.path.getmtime(r["path"])),
                "replay_ref": r["name"],
                "style": r["style"],
                "track": r["track"],
                "rank": i + 1,
                "is_invalid": False,
                "is_banned": False,
                "invalid_ref": None,
                "server": {"hostname": "local", "key_id": "local"},
                "sync": st["sync"],
                "strafes": st["strafes"],
                "jumps": st["jumps"],
                "wr_time": best.get(r["map"], r["time"]),
            })
        return out

    # ---------------------------------------------------------------- POST --

    def do_POST(self):
        u = urlparse(self.path)
        if u.path != "/api/csspak/batch":
            return self._send_json(404, {"error": "not found", "path": u.path})

        try:
            n = int(self.headers.get("Content-Length", "0"))
            paths = json.loads(self.rfile.read(n).decode("utf-8"))
            if not isinstance(paths, list):
                raise ValueError
        except (ValueError, UnicodeDecodeError):
            return self._send_json(400, {"error": "expected a JSON array of paths"})

        f = fs()
        chunks = []
        for raw in paths:
            data = f.read(raw) if isinstance(raw, str) else None
            if data is None:
                chunks.append(struct.pack("<I", CSPAK_MISSING))
            else:
                chunks.append(struct.pack("<I", len(data)))
                chunks.append(data)
        self._send(200, b"".join(chunks))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--precache", metavar="MAP", help="build the bsp.bz2 cache for MAP and exit")
    args = ap.parse_args()

    if args.precache:
        p = bsp_bz2(args.precache)
        print("cached:", p)
        return 0

    fs()
    reps = list_replays()
    print("[replay] %d file(s) visible:" % len(reps))
    for r in reps[:10]:
        print("   %-28s %6.0f KB" % (r["name"], r["size"] / 1024.0))

    # Compressing a map takes minutes on a busy machine, longer than a browser
    # waits, so every map with a replay is prepared in the background up front.
    maps = sorted(set(r["map"] for r in reps if r.get("map")))
    threading.Thread(target=lambda: [bsp_bz2(m) for m in maps], daemon=True).start()

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("\nlistening on http://%s:%d" % (args.host, args.port))
    print("  POST /api/csspak/batch   GET /api/replay?id=NAME   GET /maps/NAME.bsp.bz2")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping")
    return 0

if __name__ == "__main__":
    sys.exit(main())
