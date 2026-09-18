# cs-AI

A bot that learns to surf in Counter-Strike: Source by practising a map on its own.

It starts from a recorded run to pick up the basic technique, then repeats the map
until it gets further and faster. The approach is the same one used by the
Trackmania bots that learn a track by repetition.

## Safety

Training runs only on a local LAN server started with `-insecure`. Injecting
inputs on a normal VAC-protected server breaks Valve's rules and risks a ban. No
part of this should be pointed at a public server.

## Status

Map: surf_demise. Reference run: 39.04 seconds, set by hand.

The bot finishes the map. Eight runs from the start, one per recorded opening:

| | bot | reference |
|---|---|---|
| runs that finish | 8 of 8 | - |
| best time | 39.34 s | 39.04 s |
| median time | 39.48 s | - |
| strafe key changes | 0.76 per second | 1.05 per second |
| time at a usable strafe angle | 98.1% | 97.8% |
| median speed | 3603 units/s | 3589 units/s |

Technique matches the reference run and the remaining gap is about three tenths
of a second, spread evenly across the map rather than lost at any one point.

## Running

Run `CsAI.bat`. It opens a single window that starts and stops training, sets how
much of the machine to use, lists every process with its state, and opens replays
and reports.

Training runs several game servers alongside a learner. All of them start with
their console hidden, so the panel is the only window on screen. Double-clicking
a row shows that console; double-clicking again hides it.

Closing the panel does not stop training.

`Viewer.bat` opens a 3D replay viewer in the browser.

[HOWTO.md](HOWTO.md) covers day-to-day use in more detail.

## Power levels

Changeable at any time, including while training runs. Nothing already learned is
lost.

| setting | game servers | use |
|---|---|---|
| idle | 1 | machine in use for gaming or video |
| low | 2 | machine in use for work |
| medium | 4 | background |
| high | 6 | default |
| max | 11 | not recommended, see below |

Max is not the fastest setting. The game servers were never the bottleneck: at 11
they produce more than twice the practice the learner can read, so most of it is
discarded unread, and they take the processor time the learner needs for the
rest. Measured on 12 logical cores:

| | practice consumed per hour | discarded |
|---|---|---|
| 11 servers | 19.8 million steps | 55% |
| 6 servers | 44.3 million steps | none |

## How it works

Each attempt is one run of the whole map.

1. The recorded opening is replayed first. Building speed before the timer starts
   is a separate skill; letting the bot learn that part was tried and measured
   slower, so it replays the recording instead.
2. The bot takes over and surfs the rest.
3. It scores on distance along the route, and on time if it reaches the end. That
   score feeds back into how it steers.

In surf the only thing that matters is the angle between the held keys and the
current direction of travel. That angle is the single decision the bot makes, a
few times per second. Mouse movement and which key is held are derived from it.

Before any of that, the bot is fitted to a recorded run so it begins already
surfing. Without that step it learns to alternate keys very fast, which scores
well and looks nothing like surfing.

## Other maps

Nothing in the bot is specific to a map. Given a recorded run:

```bash
python tools/setup_map.py surf_dune
```

This derives the route, the restart points and the run to learn from. A new map
starts from scratch.

## Adding recordings

More recordings are the single most useful addition. They do not need to be fast
or clean: a run with a wobble and a recovery is worth more than another clean one,
because recovery is the case with the least coverage.

A recording is only useful if it stays near the route the bot is scored against.
On surf_demise, four of the eight stray far enough off it that the bot would be
counted as having fallen. `python tools/setup_map.py surf_demise --check` reports
which.

[HOWTO.md](HOWTO.md) covers how to record them.

## Layout

| folder | contents |
|---|---|
| `plugin/` | server plugin: drives the bot, records episodes |
| `tools/` | training, scoring, replay parsing, reports |
| `web/` | local replay viewer |
| `docs/` | record of what was tried and what worked |

Tools that are run directly:

| | |
|---|---|
| `setup_map.py <map>` | prepare a map, or `--check` an existing setup |
| `finishes.py` | finish rate and where unfinished runs stop |
| `compare.sh <gen>` | results since a change against results before it |
| `report.py` | writes `reports/latest.md` |

`docs/experiments.md` records every change and whether it helped, including the
ones that did not.

## Replay viewer

The 3D replay viewer is third-party. It is built on two repositories by offstyles:

- [offstyles/offstyles-web](https://github.com/offstyles/offstyles-web) - the site
- [offstyles/replay-viewer](https://github.com/offstyles/replay-viewer) - the in-browser map and replay renderer

Neither is included here. `web/run.ps1` fetches them on first use, and the only
modifications point them at local files instead of the live site. These are listed
in [web/README.md](web/README.md).

The renderer inside replay-viewer is a port of
[noclip.website](https://github.com/magcius/noclip.website), which is MIT licensed.

Neither offstyles repository carries a licence file, so the authors retain all
rights by default. Running a local copy is ordinary use; publishing one is not
something to do without asking them. The maps and textures it loads come from a
local game install, which is a further reason this stays a local tool.

## Requirements

- Counter-Strike: Source with a working local server
- SourceMod and Metamod on that server
- Python 3 with numpy
- The map to train on

There is no machine learning framework here. The maths is written out in numpy.
