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
| surf_demise | 38.27 s | retraining | - |
| surf_utopia_njv | 54.67 s | retraining | - |

Times run from leaving the start zone. Both records were set by hand.

The bot now has to use its hands the way a person does (see below), so it is
relearning both maps from where it was. With the old, instant view it finished
all 8 runs of an evaluation on surf_demise (median 40.08 s, fastest 39.84 s), but
it strafed like a script: short key taps and a view locked to its direction of
travel.

`CsAI.bat status` shows the current numbers.

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

The same things work from a terminal:

```bash
CsAI.bat teach surf_utopia_njv
```

| command | does |
|---|---|
| `CsAI.bat teach <map>` | learns a map from its record run and adds it to training |
| `CsAI.bat forget <map>` | takes a map out of training |
| `CsAI.bat status` | each map's record, the bot's best, and how often it finishes |
| `CsAI.bat start` / `stop` | starts or stops training |

[HOWTO.md](HOWTO.md) covers day-to-day use in more detail.

## Teaching a map

1. Set a time on the map with the timer. Segmented runs work too: loading a
   checkpoint leaves no trace in the replay, so the line is clean.
2. Run `CsAI.bat teach <map>`.

That builds the route and the restart points from the timer's replay, copies the
start zone, times the record from leaving it, adds the map to training and
restarts it. The game servers are shared out between the maps, and every map is
scored in turn. When the bot beats a record, the supervisor log says so and
`CsAI.bat status` lists it.

More recorded runs of the same map help. They do not need to be fast: a run that
wobbles and recovers teaches more than another clean one.
`python tools/setup_map.py <map> --check` shows which recordings stay close
enough to the route to be useful.

## How it works

Each attempt is one whole run of a map.

1. **Wind-up.** The bot starts standing. It walks forward with one strafe key
   down and turns towards that key, jumps well inside the start zone, and
   strafes left and right in the air until it leaves the zone. Each decision is
   a gesture: which key, and how fast to move the mouse.
2. **The run.** From leaving the zone, the surfing policy flies the rest of the
   map. It picks the strafe key and how far the view should sit off the
   direction of travel, 33 times a second. The mouse eases towards that.
3. **Scoring.** It is rewarded for distance along the route and for finishing,
   and a finish pays more the faster it is than the record. That is what pushes
   it past copying towards beating the time.

The wind-up and the run are two policies trained side by side. The wind-up is
judged on the finish time of the whole run that follows it, so it learns the
wind-up that sets up the fastest run, not the one that looks fastest at the
jump.

### Human hands

Both policies work through limits measured on the recorded runs, so there are
no tick-perfect strafes:

- The mouse has momentum. It speeds up and settles over several ticks instead of
  snapping, and never turns faster than a person does (7 degrees a tick).
- A strafe key stays down at least 12 ticks, and after letting go the next press
  waits 6.
- On a direction change the key comes up while the mouse is still turning the
  old way, as it does for a person, instead of flipping in one tick.
- The jump has to be well inside the start zone; stepping off the edge does not
  count.

`tools/humanlike.py` compares a replay with the record on the same map: mouse
speed and acceleration, how long keys are held, where the wind-up jumps and how
long it stays in the air. Every evaluation runs it and writes the result to the
supervisor log.

### Not copying

The recorded route is only a rough guide:

- Leaving the line costs very little, so a faster line of its own is not
  punished.
- It trains on several maps at once with one policy, and measures the ramps
  around it with short traces.
- Scoring, evaluations and replays always use the real line.

Shifting the line the bot is shown by a random amount each run was tried and made
it much worse; `docs/experiments.md` has the numbers.

### Keeping the best

Every few minutes each map is scored on eight runs, and the best version so far
on each map is kept.

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

Max is not faster. At 11 servers most of the practice is thrown away unread, and
the servers take the processor time the learner needs. Measured on 12 logical
cores, 6 servers got through 44.3 million steps an hour and 11 got through 19.8
million.

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

- Counter-Strike: Source with a local server. The game is found through Steam;
  if it lives somewhere Steam does not list, put its folder in `data/game.txt`
- SourceMod and Metamod on that server, with shavit's bhoptimer
- Python 3 with numpy
- A record on each map to train on

There is no machine learning framework. The maths is written out in numpy.
