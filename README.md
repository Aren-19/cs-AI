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

Maps: surf_demise, reference run 38.27 s; surf_utopia_njv, reference run 54.67 s
(both timed from leaving the start zone). Both set by hand.

On the previous surf_demise line, eight runs from the start, opened with recorded
wind-ups (training on the new line and on surf_utopia_njv has just started):

| | bot | reference |
|---|---|---|
| runs that finish | 8 of 8 | - |
| best time | 39.32 s | 39.04 s |
| median time | 39.44 s | - |
| strafe key changes | 0.76 per second | 1.05 per second |
| time at a usable strafe angle | 98.1% | 97.8% |
| median speed | 3603 units/s | 3589 units/s |

Technique matches the reference run and the remaining gap is under three tenths
of a second, spread evenly across the map rather than lost at any one point.

## Running

Run `CsAI.bat`. The first time it builds `CsAI.exe`, which opens the panel with
no console at all; either works after that.

The panel is the one window. It starts and stops training, sets how much of the
machine to use, lists every process with its log, and opens replays and reports.
Game servers, learners and supervisors all run on a separate desktop that is never
shown, so nothing else pops up.

Closing the panel while training runs asks whether to stop it.

`Viewer.bat`, or **replay viewer** in the panel, opens a 3D replay viewer in the
browser.

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

1. The bot winds up from standing. It holds forward and one strafe key at a time,
   turns its view no faster than a person does, and jumps from well inside the
   start zone, then strafes in the air before dropping off the ledge. The timer
   starts on leaving the start zone, with the combined speed capped at 475 u/s
   there, as on KSF servers.
2. From the jump, the surfing policy takes over and flies the rest of the map,
   strafing down onto the first ramp and on to the end.
3. It scores on distance along the route, and on time if it reaches the end. That
   score feeds back into how it steers.

The wind-up and the run are two policies trained side by side, in the `windup`
and `main` slots. The wind-up is scored on the finish time of the whole run the
surfing policy flies from its jump, so it learns the wind-up that sets up the
fastest run rather than one that merely looks fast. The surfing policy starts
three runs in four from the bot's own wind-up and the rest from recorded ones.

Every few minutes each slot is scored on eight runs. The best checkpoint so far
is kept, and if training falls clearly behind it for several scorings in a row
the slot rolls back to it once.

In surf the only thing that matters is the angle between the held keys and the
current direction of travel. That angle is the single decision the bot makes, a
few times per second. Mouse movement and which key is held are derived from it.

Before any of that, the bot is fitted to a recorded run so it begins already
surfing. Without that step it learns to alternate keys very fast, which scores
well and looks nothing like surfing.

## Other maps

One policy trains on several maps at once. It currently trains on surf_demise and
surf_utopia_njv (the second from a Segmented-style run: checkpoint loads leave no
trace in the replay, so the line is clean). Given a recorded run:

```bash
python tools/setup_map.py surf_utopia_njv
```

This derives the route, the restart points and the run to learn from, copies the
start zone and times the reference from leaving it. Then add the map to `-Map`
in `data/daemon_args.txt` and `data/daemon_args_windup.txt`, separated by commas.
The servers are shared out between the maps and evaluations take turns.

## Adding recordings

More recordings are the single most useful addition. They do not need to be fast
or clean: a run with a wobble and a recovery is worth more than another clean one,
because recovery is the case with the least coverage.

A recording is only useful if it stays near the route the bot is scored against.
On surf_demise, four of the eight stray far enough off it that the bot would be
counted as having fallen. `python tools/setup_map.py surf_demise --check` reports
which.

[HOWTO.md](HOWTO.md) covers how to record them.

## Timer rules

The server's timer (shavit's bhoptimer) is set up the KSF way, and training
follows the same rules:

- The timer starts on leaving the start zone, not on the jump: the Normal style
  has `startinair` 1, `nozaxisspeed` 0 and `maxprestrafe` 10000 in
  `configs/shavit-styles.cfg`. Bunnyhopping inside the zone stays blocked.
- `server/startzone-speedcap.sp` caps the combined (XYZ) speed at 475 u/s on
  leaving the zone (`startzone_speedcap`).
- Replays keep 4 seconds before the zone is left (`shavit_replay_preruntime`,
  whose limit was raised from 2 to 10 in `shavit-replay-recorder.sp`), so the
  replay bot shows the wind-up.

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

Neither is included here. `web/run.ps1` fetches them at a fixed commit on first
use and applies the small changes in `web/patches/`, which point them at local
files instead of the live site. These are listed in
[web/README.md](web/README.md).

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
