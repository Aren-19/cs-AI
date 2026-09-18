"""Summary numbers for a replay file."""

import math
import os
import struct

IN_JUMP = 2
IN_MOVELEFT = 512
IN_MOVERIGHT = 1024
FL_ONGROUND = 1

_cache = {}

def _frames(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    nl = blob.index(bytes([10]))
    version = int(blob[:nl].split(b":")[0])
    p = nl + 1
    pre = post = 0
    if version >= 3:
        e = blob.index(bytes([0]), p)
        p = e + 1
        p += 2                       # style, track
        pre = struct.unpack_from("<i", blob, p)[0]; p += 4
        if pre < 0:
            pre = 0
    count = struct.unpack_from("<i", blob, p)[0]; p += 4
    p += 4                           # time
    if version < 7:
        count -= pre
    if version >= 4:
        p += 4                       # steamid
    if version >= 5:
        post = struct.unpack_from("<i", blob, p)[0]; p += 4
        p += 4                       # tickrate
        if version < 7:
            count -= post
    if version >= 8:
        p += 8                       # zone offsets

    remaining = len(blob) - p
    total = count + pre + post
    if total <= 0:
        return []
    size = remaining // total
    if size <= 0:
        return []

    out = []
    for i in range(total):
        o = p + i * size
        if o + 24 > len(blob):
            break
        yaw = struct.unpack_from("<f", blob, o + 16)[0]
        buttons = struct.unpack_from("<i", blob, o + 20)[0] if size >= 24 else 0
        flags = struct.unpack_from("<i", blob, o + 24)[0] if size >= 28 else 0
        out.append((yaw, buttons, flags))
    return out

def stats(path):
    key = (path, os.path.getmtime(path))
    hit = _cache.get(key)
    if hit is not None:
        return hit

    try:
        fr = _frames(path)
    except (OSError, ValueError, struct.error):
        fr = []

    jumps = strafes = 0
    good = total = 0
    prev_buttons = 0
    prev_yaw = None

    for yaw, buttons, flags in fr:
        if (buttons & IN_JUMP) and not (prev_buttons & IN_JUMP):
            jumps += 1
        for bit in (IN_MOVELEFT, IN_MOVERIGHT):
            if (buttons & bit) and not (prev_buttons & bit):
                strafes += 1

        if prev_yaw is not None and not (flags & FL_ONGROUND)                 and math.isfinite(yaw) and math.isfinite(prev_yaw):
            # math.remainder is branchless and safe; the old while-loops spun
            # forever on a non-finite delta.
            d = math.remainder(yaw - prev_yaw, 360.0)
            left = bool(buttons & IN_MOVELEFT)
            right = bool(buttons & IN_MOVERIGHT)
            if left != right and abs(d) > 1e-4:
                total += 1
                # +yaw is a left turn in Source; gaining speed needs the key and
                # the turn to agree
                if (left and d > 0) or (right and d < 0):
                    good += 1

        prev_buttons = buttons
        prev_yaw = yaw

    out = {
        "jumps": jumps,
        "strafes": strafes,
        "sync": (100.0 * good / total) if total else 0.0,
        "frames": len(fr),
    }
    _cache[key] = out
    return out

if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        s = stats(p)
        print("%-42s jumps %-4d strafes %-5d sync %5.1f%%  frames %d"
              % (os.path.basename(p), s["jumps"], s["strafes"], s["sync"], s["frames"]))
