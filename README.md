# cs-AI

A bot that teaches itself to surf in Counter-Strike: Source.

It watches your recorded runs to learn the basic technique, then practises the
map over and over on its own, trying to get further and faster each time. The
idea is the same as the Trackmania bots that learn a track by repetition.

On surf_demise it now gets to the end, in about 39.6 seconds against a hand-made
39.04.

## Read this first

Training only ever runs on a local LAN server started with `-insecure`. Feeding
inputs to a normal, VAC-protected server would break Valve's rules and can get
your account banned. Nothing here should ever be pointed at a public server.

## Where it is right now

Map: surf_demise. The run to beat is 39.04 seconds, set by hand.

It finishes the map. Out of eight runs from the start, seven get to the end, and
the times sit in a tight band:

| | bot | the human run |
|---|---|---|
| finishes the map | 7 runs out of 8 | yes |
| best time | 39.59 s | 39.04 s |
| typical time | 39.67 s | - |
| strafe key changes | 0.81 per second | 1.05 per second |
| time spent at a useful angle | 98.2% | 97.8% |
| speed it holds | 3610 units/s | 3589 units/s |

So it surfs about as well as a person does, and it is roughly half a second off
the pace. The key-swapping problem it used to have is gone: it used to change
keys eight times a second, which scored well and looked nothing like surfing.

What is left is the last half second, and that is harder than everything before
it. Watching the runs, the bot is not losing time in any one place - it is a
fraction slower everywhere.

## Running it

Double-click `CsAI.bat`. That is the whole thing: one window that starts and
stops training, changes how much of your computer it uses, lists everything that
is running, and opens replays and reports.

Training runs several game servers plus a learner in the background. They all
start hidden, so you get one window instead of a screen full of them. If you want
to look inside one, double-click its row in the list and that console appears;
double-click again and it goes away.

Closing the panel does not stop training. It keeps going in the background, so
you can start it and leave.

`Viewer.bat` opens a 3D replay viewer in your browser so you can watch what the
bot actually did.

There is more detail in [HOWTO.md](HOWTO.md).

## How much of your PC it uses

You can change this at any time, even while it is running, and nothing already
learned is lost.

| setting | game servers | when to use it |
|---|---|---|
| idle | 1 | you are gaming or watching something |
| low | 2 | you are working on the PC |
| medium | 4 | background |
| high | 6 | leave it here |
| max | 11 | see below |

**Max is not the fastest setting.** It was, on paper, and that turned out to be
wrong when it was finally measured. The game servers were never the slow part.
They were already producing more than twice the practice runs the learner could
read, so most of it was thrown away unread, and the extra servers took the
processor time the learner needed to read the rest. On a 12 core machine:

| | practice read per hour | thrown away |
|---|---|---|
| 11 servers | 19.8 million steps | 55% |
| 6 servers | 44.3 million steps | none |

Twice the learning on half the machine. High is the default and there is no
reason to move off it.

## How it works

Every attempt is one run of the whole map, start to finish.

1. Your recorded start is replayed first. Building up speed before the timer
   starts is a different skill to surfing, and letting the bot learn that part
   too was tried and made it slower, so it copies yours.
2. The bot takes over and surfs the rest.
3. It gets a score for how far along the map it travelled, and for how long it
   took if it got to the end. A little of that score feeds back into how it
   steers next time.

When you surf, the only thing that really matters is the angle between the way
you are holding your keys and the way you are already moving. So that angle is
the one thing the bot decides, a few times per second. Everything else, the mouse
movement and which key is held, is worked out from that angle afterwards.

Before any of this, the bot copies a run of yours directly, so it starts out
already surfing rather than flailing around. Without that first step it learns to
twitch left and right very fast, which scores well but looks nothing like real
surfing.

## Using it on another map

Nothing in the bot knows about a particular map. Point it at one you have a
recorded run for:

```bash
python tools/setup_map.py surf_dune
```

That works out the route, the restart points and the run to learn from, all from
your replay. The bot starts from nothing on a new map, the same way it did on the
first one.

## Teaching it with your own runs

This is the most useful thing you can do for it.

It has eight runs of yours on surf_demise now, and that made a real difference.
More still help, and they do not need to be fast or clean. A messy run where you
wobble and recover is worth more than another perfect one, because recovering is
the part it sees least of.

One thing worth knowing: a run is only useful if it stays near the route the bot
is scored against. Four of the eight stray far enough off it that the bot would
be counted as having fallen. They are fine to keep, but they are not good ones to
copy technique from. `python tools/setup_map.py surf_demise --check` says which
is which.

See [HOWTO.md](HOWTO.md) for how to add them.

## What is in here

| folder | what it is |
|---|---|
| `plugin/` | the server plugin that drives the bot and records what happened |
| `tools/` | training, scoring, replay reading, reports |
| `web/` | the local replay viewer |
| `docs/` | notes on what was tried and what worked |

A few things in `tools/` you might run by hand:

| | |
|---|---|
| `setup_map.py <map>` | get a map ready from your replays, or `--check` what is wrong |
| `finishes.py` | how often it gets to the end, and where the rest stop |
| `compare.sh <gen>` | how the run since a change compares to before it |
| `report.py` | writes `reports/latest.md` |

`docs/experiments.md` is a running log of every change and whether it helped,
including the ones that did not, which is most of them.

## Credit for the viewer

The 3D replay viewer is not mine. It is built on two repos by offstyles, the
people behind offstyles.net:

- [offstyles/offstyles-web](https://github.com/offstyles/offstyles-web) - the site itself
- [offstyles/replay-viewer](https://github.com/offstyles/replay-viewer) - the in-browser map and replay renderer

Neither is included here. `web/run.ps1` fetches them when you first run the
viewer, and the only changes made are small ones to point them at your own files
instead of the live site. Those changes are listed in [web/README.md](web/README.md).

The renderer inside replay-viewer is a port of
[noclip.website](https://github.com/magcius/noclip.website), which is MIT
licensed.

One thing to be aware of: neither offstyles repo has a licence file, so by
default the authors keep all rights. Running a copy locally for yourself is
normal. Putting a copy online is not something to do without asking them first.
The map and texture files it loads are Valve's and come from your own game
install, which is another reason this stays a local tool.

## What you need

- Counter-Strike: Source, with a working local server
- SourceMod and Metamod on that server
- Python 3 with numpy
- The map you want to train on

There is no machine learning library here. The maths is written out in plain
numpy, so you can read it.
