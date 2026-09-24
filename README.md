# cs-AI

A bot that learns to surf in Counter-Strike: Source by practising maps on its own.

The goal is not to copy a recorded run but to finish maps and beat the record. A
recorded run shows it roughly where the route goes; everything else, from the
wind-up at the start to the line down each ramp, it works out by repeating the
maps and keeping what makes it faster.

## Safety

Training runs only on a local LAN server started with `-insecure`. Injecting
inputs on a normal VAC-protected server breaks Valve's rules and risks a ban. No
part of this should be pointed at a public server.

## Status

| map | record to beat | bot, best so far | finishing |
|---|---|---|---|
| surf_demise | 38.27 s | 39.84 s | 7 of 8 runs |
| surf_utopia_njv | 54.67 s | - | still learning the map |

Times run from leaving the start zone. Both records were set by hand.

On the previous surf_demise line the bot finished 8 of 8 runs with a best of 39.32
s against a 39.04 s record, with the same strafing technique as the person who set
it. Moving to a new line broke it completely, which showed it had learned the
route as much as the skill. Training now runs on two maps at once with the line
deliberately blurred (see below), so it has to learn surfing rather than one
route.

## Running

Run `CsAI.bat`. The first time, it builds `CsAI.exe`, which opens the panel with
no console at all. Either works after that.

The panel is the only window. From it:

- start and stop training
- set how much of the machine training may use
- see every process, what it is doing, and its log
- see each map's finish rate, median time and best result
- open the replay viewer and the latest report

Game servers, learners and supervisors run on a separate desktop that is never
shown, so nothing else pops up. Closing the panel while training runs asks
whether to stop it first.

[HOWTO.md](HOWTO.md) covers day-to-day use in more detail.

## How it works

Each attempt is one whole run of a map.

1. **Wind-up.** The bot stands at the start, holds forward and one strafe key at
   a time, and turns its view no faster than a person can. It jumps from well
   inside the start zone and strafes in the air before dropping off the ledge.
   Walking to the edge of the zone and stepping off does not count.
2. **The run.** From the jump, the surfing policy flies the rest of the map. In
   surf the one thing that matters is the angle between the held key and the
   direction of travel, so that angle is the decision it makes, 33 times a
   second. The mouse and keys follow from it.
3. **Scoring.** It is rewarded for distance along the route and for finishing,
   and a finish pays more the faster it is than the record. That is what pushes
   it past copying towards beating the time.

The wind-up and the run are two policies trained side by side. The wind-up is
judged on the finish time of the whole run that follows it, so it learns the
wind-up that sets up the fastest run, not the one that looks fastest at the
jump.

### Not copying

The recorded route is only a rough guide:

- In training, the line the bot is shown is shifted by a random slow wave every
  run, up to 300 units sideways and 150 up or down. It cannot rely on the exact
  line, so it has to read the ramps around it, which it measures with short
  traces.
- Leaving the line costs very little, so a faster line of its own is not
  punished.
- It trains on several maps at once with one policy.

Scoring, evaluations and replays always use the real line.

### Keeping the best

Every few minutes each map is scored on eight runs. The best version so far is
kept, and if training falls clearly behind it for several scorings in a row, it
goes back to that version once.

## Maps

One policy trains on every map listed in `data/daemon_args.txt`. To add a map,
set a record on it, then:

```bash
python tools/setup_map.py surf_utopia_njv
```

This builds the route and the restart points from the timer's replay, copies the
start zone, and times the record from leaving it. Then add the map to `-Map` in
`data/daemon_args.txt` and `data/daemon_args_windup.txt`, separated by commas,
and press **start**. The game servers are shared out between the maps.

Segmented runs work too: loading a checkpoint leaves no trace in the replay, so
the line is clean.

More recorded runs of the same map help as well. They do not need to be fast: a
run that wobbles and recovers teaches more than another clean one.
`python tools/setup_map.py <map> --check` shows which recordings stay close
enough to the route to be useful.

## Timer rules

The server's timer (shavit's bhoptimer) is set up the way KSF servers run it, and
training follows the same rules:

- The timer starts on leaving the start zone, not on the jump.
- Combined speed is capped at 475 u/s on leaving the start zone
  (`server/startzone-speedcap.sp`).
- Bunnyhopping inside the start zone is blocked.
- Replays keep the 4 seconds before the start zone is left, so the replay bot
  shows the wind-up.

## Power levels

Changeable at any time, even while training runs. Nothing learned is lost.

| setting | game servers | use |
|---|---|---|
| idle | 1 | gaming or video on the same machine |
| low | 2 | working on the same machine |
| medium | 4 | background |
| high | 6 | default |
| max | 11 | not recommended |

Max is not faster. The learner, not the game servers, is the bottleneck: at 11
servers most of the practice is thrown away unread, and the servers take the
processor time the learner needs. Measured on 12 logical cores, 6 servers got
through 44.3 million steps an hour and 11 got through 19.8 million.

## Layout

| folder | contents |
|---|---|
| `plugin/` | server plugin: drives the bot and records what it does |
| `tools/` | training, scoring, replay reading, reports, the panel |
| `server/` | changes to the timer that training depends on |
| `web/` | local replay viewer |
| `docs/` | what was tried and whether it worked |

`docs/experiments.md` records every change and its result, including the ones
that did not help.

## Replay viewer

The 3D replay viewer is built on two repositories by offstyles:

- [offstyles/offstyles-web](https://github.com/offstyles/offstyles-web) - the site
- [offstyles/replay-viewer](https://github.com/offstyles/replay-viewer) - the in-browser map and replay renderer

Neither is included here. `web/run.ps1` fetches them at a fixed commit the first
time and applies the small changes in `web/patches/`, which point them at local
files instead of the live site. [web/README.md](web/README.md) lists them.

The renderer inside replay-viewer is a port of
[noclip.website](https://github.com/magcius/noclip.website), which is MIT licensed.
The offstyles repositories carry no licence file, so their authors keep all
rights: running a local copy is fine, publishing one is not without asking them.
The maps and textures come from the local game install.

## Requirements

- Counter-Strike: Source with a local server
- SourceMod and Metamod on that server, with shavit's bhoptimer
- Python 3 with numpy
- A record on each map to train on

There is no machine learning framework. The maths is written out in numpy.
