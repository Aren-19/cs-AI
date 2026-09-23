"""Turn a timer replay into the track, start states, prestrafe and demo files."""

import argparse
import json
import math
import os
import struct
import sys

# IN_* button bits (Source in_buttons.h)
IN_ATTACK, IN_JUMP, IN_DUCK, IN_FORWARD, IN_BACK = 1, 2, 4, 8, 16
IN_USE, IN_MOVELEFT, IN_MOVERIGHT = 32, 512, 1024

FRAME_SIZE_V6 = 40
FRAME_SIZE_V2 = 28          # pos, ang, buttons, flags, movetype
FRAME_SIZE_V1 = 24          # pos, ang, buttons

def unpack_signed_shorts(x):
    """shavit packs two signed shorts into one int32."""
    lo = ((x & 0xFFFF) ^ 0x8000) - 0x8000
    hi = (((x >> 16) & 0xFFFF) ^ 0x8000) - 0x8000
    return lo, hi

class Frame(object):
    __slots__ = ("pos", "ang", "buttons", "flags", "movetype",
                 "mousex", "mousey", "forwardmove", "sidemove")

    def __init__(self, pos, ang, buttons, flags, movetype,
                 mousex=0, mousey=0, forwardmove=0, sidemove=0):
        self.pos = pos
        self.ang = ang
        self.buttons = buttons
        self.flags = flags
        self.movetype = movetype
        self.mousex = mousex
        self.mousey = mousey
        self.forwardmove = forwardmove
        self.sidemove = sidemove

class Replay(object):
    def __init__(self):
        self.version = 0
        self.fmt = ""
        self.map = ""
        self.style = 0
        self.track = 0
        self.preframes = 0
        self.frame_count = 0
        self.time = 0.0
        self.steamid = 0
        self.postframes = 0
        self.tickrate = 0.0
        self.zone_offset = (0.0, 0.0)
        self.frames = []          # all frames, including pre/post

    @property
    def run_frames(self):
        """Just the timed portion - pre/post frames are idle padding."""
        return self.frames[self.preframes:self.preframes + self.frame_count]

def parse_replay(path):
    with open(path, "rb") as fh:
        d = fh.read()

    r = Replay()
    p = d.index(b"\n")
    header_line = d[:p].decode("ascii", "replace").strip()
    p += 1

    parts = header_line.split(":")
    r.version = int(parts[0])
    r.fmt = parts[1] if len(parts) > 1 else ""

    if r.version >= 3:
        end = d.index(b"\x00", p)
        r.map = d[p:end].decode("ascii", "replace")
        p = end + 1
        r.style = d[p]; p += 1
        r.track = d[p]; p += 1
        r.preframes = struct.unpack_from("<i", d, p)[0]; p += 4
        if r.preframes < 0:
            r.preframes = 0

    r.frame_count = struct.unpack_from("<i", d, p)[0]; p += 4
    r.time = struct.unpack_from("<f", d, p)[0]; p += 4

    if r.version < 7:
        r.frame_count -= r.preframes
    if r.version >= 4:
        r.steamid = struct.unpack_from("<i", d, p)[0]; p += 4
    if r.version >= 5:
        r.postframes = struct.unpack_from("<i", d, p)[0]; p += 4
        r.tickrate = struct.unpack_from("<f", d, p)[0]; p += 4
        if r.version < 7:
            r.frame_count -= r.postframes
    if r.version >= 8:
        z0 = struct.unpack_from("<f", d, p)[0]; p += 4
        z1 = struct.unpack_from("<f", d, p)[0]; p += 4
        r.zone_offset = (z0, z1)

    total = r.frame_count + r.preframes + r.postframes
    remaining = len(d) - p
    if total <= 0:
        raise ValueError("replay declares %d frames" % total)

    frame_size = remaining // total
    if frame_size * total != remaining:
        raise ValueError(
            "non-integer frame size: %d bytes left over %d frames (%.3f). "
            "Header layout is wrong for version %d." % (remaining, total, remaining / total, r.version))

    expected = FRAME_SIZE_V6 if r.version >= 6 else (
        FRAME_SIZE_V2 if r.version >= 2 else FRAME_SIZE_V1)
    if frame_size != expected:
        sys.stderr.write("warning: frame size %d, expected %d for version %d\n"
                         % (frame_size, expected, r.version))

    frames = []
    for i in range(total):
        o = p + i * frame_size
        x, y, z, pitch, yaw = struct.unpack_from("<5f", d, o)
        buttons = struct.unpack_from("<i", d, o + 20)[0] if frame_size >= 24 else 0
        flags = movetype = 0
        mx = my = fm = sm = 0
        if frame_size >= 28:
            flags, movetype = struct.unpack_from("<2i", d, o + 24)
        if frame_size >= 40:
            mousexy, vel = struct.unpack_from("<2i", d, o + 32)
            mx, my = unpack_signed_shorts(mousexy)
            fm, sm = unpack_signed_shorts(vel)
        frames.append(Frame((x, y, z), (pitch, yaw), buttons, flags, movetype, mx, my, fm, sm))

    r.frames = frames
    return r

# ------------------------------------------------------------------ derived ----

def _dist(a, b):
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)

TELEPORT_SPEED = 12000.0       # u/s

def teleport_threshold(tickrate):
    """Per-tick distance that counts as a teleport, for this replay's tickrate."""
    tr = tickrate if tickrate and tickrate > 0 else 100.0
    return TELEPORT_SPEED / tr

def path_length(frames, tickrate=100.0):
    thr = teleport_threshold(tickrate)
    total = 0.0
    for i in range(1, len(frames)):
        d = _dist(frames[i - 1].pos, frames[i].pos)
        if d <= thr:
            total += d
    return total

def segments(frames, tickrate=100.0, threshold=None):
    """Split the run at teleports, which staged maps use between stages."""
    if not frames:
        return []
    if threshold is None:
        threshold = teleport_threshold(tickrate)
    out = []
    start = 0
    for i in range(1, len(frames)):
        if _dist(frames[i - 1].pos, frames[i].pos) > threshold:
            out.append((start, i))
            start = i
    out.append((start, len(frames)))
    return out

def prune_failed_attempts(frames, segs, tol=64.0):
    """Segments that begin at the same teleport destination are retries; keep the last."""
    kept = []
    for si, (a, b) in enumerate(segs):
        start_pos = frames[a].pos
        superseded = False
        for (a2, _) in segs[si + 1:]:
            if _dist(frames[a2].pos, start_pos) <= tol:
                superseded = True
                break
        if not superseded:
            kept.append((a, b))
    return kept

def clean_frames(frames, tickrate=100.0):
    """The frames that belong to the successful line through the map."""
    segs = prune_failed_attempts(frames, segments(frames, tickrate))
    out = []
    for a, b in segs:
        out.extend(frames[a:b])
    return out, segs

def resample(frames, src_rate, dst_rate):
    """Re-time a recording onto another tickrate."""
    if not frames or src_rate <= 0 or dst_rate <= 0:
        return list(frames)
    if abs(src_rate - dst_rate) < 0.01:
        return list(frames)
    n_out = max(1, int(round(len(frames) * dst_rate / src_rate)))
    out = []
    for j in range(n_out):
        i = int(round(j * src_rate / dst_rate))
        out.append(frames[min(i, len(frames) - 1)])
    return out

def centerline(frames, spacing=64.0, jump=200.0):
    """Resample the run by arc length, and say where it teleports."""
    if not frames:
        return [], []
    pts = [frames[0].pos]
    brk = [False]
    acc = 0.0
    for i in range(1, len(frames)):
        seg = _dist(frames[i - 1].pos, frames[i].pos)
        if seg > jump:
            # A teleport. Start the next stage as its own point and do not count
            # the gap as distance travelled.
            if pts[-1] != frames[i - 1].pos:
                pts.append(frames[i - 1].pos)
                brk.append(False)
            brk[-1] = True
            pts.append(frames[i].pos)
            brk.append(False)
            acc = 0.0
            continue
        acc += seg
        if acc >= spacing:
            pts.append(frames[i].pos)
            brk.append(False)
            acc -= spacing
    if pts[-1] != frames[-1].pos:
        pts.append(frames[-1].pos)
        brk.append(False)
    return pts, brk

def checkpoints(frames, count=24, tickrate=100.0):
    """Evenly spaced full states (position, angles, velocity) to restart from."""
    if len(frames) < 2:
        return []
    dt = 1.0 / tickrate if tickrate > 0 else 0.01
    out = []
    n = len(frames)
    for k in range(count):
        i = int(round(k * (n - 1) / float(max(count - 1, 1))))
        i = max(1, min(i, n - 1))
        prev, cur = frames[i - 1], frames[i]
        if _dist(prev.pos, cur.pos) > teleport_threshold(tickrate):
            # landed on a teleport boundary; step forward to stay inside a segment
            if i + 1 < n:
                prev, cur, i = cur, frames[i + 1], i + 1
            else:
                continue
        vel = tuple((cur.pos[j] - prev.pos[j]) / dt for j in range(3))
        out.append({
            "index": i,
            "frac": i / float(n - 1),
            "time": i * dt,
            "origin": list(cur.pos),
            "angles": [cur.ang[0], cur.ang[1]],
            "velocity": list(vel),
            "flags": cur.flags,
            "movetype": cur.movetype,
        })
    return out

def speed_profile(frames, tickrate=100.0):
    dt = 1.0 / tickrate if tickrate > 0 else 0.01
    speeds = []
    for i in range(1, len(frames)):
        a, b = frames[i - 1].pos, frames[i].pos
        if _dist(a, b) > teleport_threshold(tickrate):
            continue                      # teleport, not motion
        speeds.append(math.sqrt((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) / dt)
    return speeds

def button_names(b):
    names = []
    for bit, nm in ((IN_JUMP, "JUMP"), (IN_DUCK, "DUCK"), (IN_FORWARD, "FWD"),
                    (IN_BACK, "BACK"), (IN_MOVELEFT, "LEFT"), (IN_MOVERIGHT, "RIGHT")):
        if b & bit:
            names.append(nm)
    return "+".join(names) if names else "-"

# ---------------------------------------------------------------------- cli ----

def main():
    ap = argparse.ArgumentParser(description="Parse a shavit replay and derive training artefacts")
    ap.add_argument("path")
    ap.add_argument("--out", help="directory to write centerline.csv / checkpoints.json / frames.csv")
    ap.add_argument("--spacing", type=float, default=64.0, help="centerline resample spacing in units")
    ap.add_argument("--checkpoints", type=int, default=24, help="number of curriculum checkpoints")
    ap.add_argument("--states", help="write a flat states file here for the SourcePawn harness "
                                     "(one state per line; SourcePawn has no JSON parser)")
    ap.add_argument("--track", help="write the dense centerline with cumulative arc length here, "
                                    "flat text for the SourcePawn harness")
    ap.add_argument("--prestrafe", help="write the recorded pre-timer inputs here, for the plugin "
                                        "to replay before handing control to the policy")
    ap.add_argument("--demo", help="write the FULL recorded run (preframes + run) here, for the "
                                   "plugin to replay while capturing behaviour-cloning data")
    ap.add_argument("--tickrate", type=float, default=66.67,
                    help="tickrate the SERVER runs at. Recordings are re-timed to "
                         "it: the plugin replays one recorded tick per server "
                         "tick, so a 100-tick replay on a 66-tick server holds "
                         "every input 1.5x too long and flies a different line.")
    args = ap.parse_args()

    r = parse_replay(args.path)
    raw = r.run_frames
    tr = r.tickrate if r.tickrate > 0 else 100.0
    run, kept_segs = clean_frames(raw, tr)
    all_segs = segments(raw, tr)
    speeds = speed_profile(run, tr)

    print("map            : %s   style %d  track %d" % (r.map, r.style, r.track))
    print("version        : %d  (%s)" % (r.version, r.fmt))
    print("time           : %.3f s   tickrate %.0f" % (r.time, tr))
    print("frames         : %d run  (+%d pre, +%d post)" % (r.frame_count, r.preframes, r.postframes))
    print("zone offsets   : %.3f / %.3f" % r.zone_offset)
    print("path length    : %.0f units (teleports excluded)" % path_length(run, tr))
    print("teleport thresh: %.0f units/tick (%.0f u/s at %.0f tick)"
          % (teleport_threshold(tr), TELEPORT_SPEED, tr))
    print("segments       : %d total, %d kept after pruning failed attempts"
          % (len(all_segs), len(kept_segs)))
    if len(all_segs) != len(kept_segs):
        dropped = len(all_segs) - len(kept_segs)
        print("                 %d segment(s) dropped: reset to a stage start after falling"
              % dropped)
    for a, b in all_segs:
        mark = "keep" if (a, b) in kept_segs else "DROP"
        print("                 [%s] frames %5d..%-5d (%5d)  start (%.0f %.0f %.0f)"
              % (mark, a, b, b - a, raw[a].pos[0], raw[a].pos[1], raw[a].pos[2]))
    print("clean frames   : %d of %d (%.1f%% of the recording is the successful line)"
          % (len(run), len(raw), 100.0 * len(run) / max(len(raw), 1)))
    print("clean time     : %.2f s  <- a no-death run of this quality; the real bar"
          % (len(run) / tr))

    if speeds:
        srt = sorted(speeds)
        print("h-speed        : mean %.0f  median %.0f  p95 %.0f  max %.0f u/s"
              % (sum(speeds) / len(speeds), srt[len(srt) // 2],
                 srt[int(len(srt) * 0.95)], srt[-1]))

    if run:
        f0, fN = run[0], run[-1]
        print("start          : (%.0f %.0f %.0f) yaw %.1f" % (f0.pos[0], f0.pos[1], f0.pos[2], f0.ang[1]))
        print("end            : (%.0f %.0f %.0f)" % (fN.pos[0], fN.pos[1], fN.pos[2]))

    held = sum(1 for f in run if f.buttons & (IN_MOVELEFT | IN_MOVERIGHT))
    both = sum(1 for f in run if (f.buttons & IN_MOVELEFT) and (f.buttons & IN_MOVERIGHT))
    print("strafe keys    : %d/%d frames (%.1f%%), both-at-once %d"
          % (held, len(run), 100.0 * held / max(len(run), 1), both))

    cl, cl_brk = centerline(run, args.spacing)
    cps = checkpoints(run, args.checkpoints, tr)
    print("centerline     : %d points at %.0f-unit spacing" % (len(cl), args.spacing))
    print("checkpoints    : %d" % len(cps))

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        base = os.path.splitext(os.path.basename(args.path))[0]

        with open(os.path.join(args.out, base + "_centerline.csv"), "w") as fh:
            fh.write("i,x,y,z\n")
            for i, pt in enumerate(cl):
                fh.write("%d,%.3f,%.3f,%.3f\n" % (i, pt[0], pt[1], pt[2]))

        with open(os.path.join(args.out, base + "_checkpoints.json"), "w") as fh:
            json.dump({"map": r.map, "style": r.style, "track": r.track,
                       # recorded_time includes the failed attempts the timer ran through;
                       # clean_time is the successful line only, and is the honest bar.
                       "recorded_time": r.time,
                       "clean_time": len(run) / tr,
                       "clean_frames": len(run),
                       "raw_frames": len(raw),
                       "tickrate": tr,
                       "checkpoints": cps}, fh, indent=2)

        with open(os.path.join(args.out, base + "_frames.csv"), "w") as fh:
            fh.write("tick,x,y,z,pitch,yaw,buttons,btn_names,flags,movetype,mousex,mousey,forwardmove,sidemove\n")
            for i, f in enumerate(run):
                fh.write("%d,%.3f,%.3f,%.3f,%.2f,%.2f,%d,%s,%d,%d,%d,%d,%d,%d\n"
                         % (i, f.pos[0], f.pos[1], f.pos[2], f.ang[0], f.ang[1],
                            f.buttons, button_names(f.buttons), f.flags, f.movetype,
                            f.mousex, f.mousey, f.forwardmove, f.sidemove))

        print("wrote          : %s_{centerline.csv,checkpoints.json,frames.csv} -> %s"
              % (base, args.out))

    if args.states:
        # Flat, space-separated, one state per line. SourcePawn has no JSON
        # parser, and this is trivially read with File.ReadLine + ExplodeString.
        d = os.path.dirname(args.states)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(args.states, "w") as fh:
            print("# csai states v1  map=%s tickrate=%.2f clean_time=%.3f"
                  % (r.map, tr, len(run) / tr), file=fh)
            print("# idx frac time x y z pitch yaw vx vy vz flags movetype", file=fh)
            for k, c in enumerate(cps):
                print("%d %.6f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %d %d"
                      % (k, c["frac"], c["time"],
                         c["origin"][0], c["origin"][1], c["origin"][2],
                         c["angles"][0], c["angles"][1],
                         c["velocity"][0], c["velocity"][1], c["velocity"][2],
                         c["flags"], c["movetype"]), file=fh)
        print("states file    : %s (%d states)" % (args.states, len(cps)))

    if args.track:
        # Dense centerline with cumulative arc length, flat text for SourcePawn.
        # Progress along this polyline is the dense reward signal.
        d = os.path.dirname(args.track)
        if d:
            os.makedirs(d, exist_ok=True)
        # Arc length skips the teleport gaps: the distance across one is not
        # travel and must not be rewarded as progress.
        cum = [0.0]
        for i in range(1, len(cl)):
            step = 0.0 if cl_brk[i - 1] else _dist(cl[i - 1], cl[i])
            cum.append(cum[-1] + step)
        with open(args.track, "w") as fh:
            print("# csai track v1  map=%s tickrate=%.2f clean_time=%.3f points=%d length=%.1f"
                  % (r.map, tr, len(run) / tr, len(cl), cum[-1]), file=fh)
            print("# idx x y z cum_s", file=fh)
            for i, pt in enumerate(cl):
                print("%d %.3f %.3f %.3f %.3f" % (i, pt[0], pt[1], pt[2], cum[i]), file=fh)
        print("track file     : %s (%d points, %.0f units)" % (args.track, len(cl), cum[-1]))

    if args.prestrafe:
        pre = resample(r.frames[:r.preframes], tr, args.tickrate)
        d = os.path.dirname(args.prestrafe)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(args.prestrafe, "w") as fh:
            print("# csai prestrafe v1  map=%s tickrate=%.2f ticks=%d"
                  % (r.map, args.tickrate, len(pre)), file=fh)
            print("# idx x y z pitch yaw buttons flags movetype", file=fh)
            for i, f in enumerate(pre):
                print("%d %.4f %.4f %.4f %.4f %.4f %d %d %d"
                      % (i, f.pos[0], f.pos[1], f.pos[2], f.ang[0], f.ang[1],
                         f.buttons, f.flags, f.movetype), file=fh)
        if len(pre) >= 2:
            a, b = pre[-2], pre[-1]
            sp = math.hypot((b.pos[0]-a.pos[0]) * args.tickrate,
                            (b.pos[1]-a.pos[1]) * args.tickrate)
            print("prestrafe      : %s (%d ticks, exits at %.0f u/s)"
                  % (args.prestrafe, len(pre), sp))
        else:
            print("prestrafe      : %s (%d ticks)" % (args.prestrafe, len(pre)))

    if args.demo:
        d = os.path.dirname(args.demo)
        if d:
            os.makedirs(d, exist_ok=True)
        pre_n = len(resample(r.frames[:r.preframes], tr, args.tickrate))
        allf = (resample(r.frames[:r.preframes], tr, args.tickrate)
                + resample(run, tr, args.tickrate))
        with open(args.demo, "w") as fh:
            print("# csai demo v1  map=%s tickrate=%.2f preframes=%d total=%d"
                  % (r.map, args.tickrate, pre_n, len(allf)), file=fh)
            print("# idx x y z pitch yaw buttons", file=fh)
            for i, f in enumerate(allf):
                print("%d %.4f %.4f %.4f %.4f %.4f %d"
                      % (i, f.pos[0], f.pos[1], f.pos[2], f.ang[0], f.ang[1], f.buttons), file=fh)
        print("demo file      : %s (%d ticks, %d preframes)" % (args.demo, len(allf), r.preframes))

if __name__ == "__main__":
    main()
