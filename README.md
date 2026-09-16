# cs-AI

A bot that teaches itself to surf in Counter-Strike: Source.

It watches one of your recorded runs to learn the basic technique, then practises
the map over and over on its own, trying to get further and faster each time. The
idea is the same as the Trackmania bots that learn a track by repetition.

## Read this first

Training only ever runs on a local LAN server started with `-insecure`. Feeding
inputs to a normal, VAC-protected server would break Valve's rules and can get
your account banned. Nothing here should ever be pointed at a public server.

## Where it is right now

Map: surf_demise. Human time to beat: 39.10 seconds.

| | best run | human |
|---|---|---|
| how far it gets | about 60% of the map | finishes |
| top speed it holds | 3298 units/s | 3614 units/s |
| strafe key changes | 8.35 per second | 0.95 per second |

So it surfs, it is nearly as fast as a person, and it gets most of the way
through. It does not finish yet, and it still swaps between the A and D keys far
more often than a person would. Both are being worked on.

## Running it

Double-click `CsAI.bat`. That is the whole thing. It gives you a menu to start
and stop training, change how much of your computer it uses, watch replays and
read reports.

Closing that window does not stop training. It keeps going in the background, so
you can start it and leave.

`Viewer.bat` opens a 3D replay viewer in your browser so you can watch what the
bot actually did.

There is more detail in [HOWTO.md](HOWTO.md).

## How much of your PC it uses

You can change this at any time, even while it is running, and nothing already
learned is lost.

| setting | when to use it |
|---|---|
| idle | you are gaming or watching something |
| low | you are working on the PC |
| medium | background |
| high | default |
| max | you are away from the PC |

Training runs several copies of the server side by side to go faster. On a 12
core machine that is about five times quicker than running one.

## How it works

Every attempt is one run of the whole map, start to finish.

1. Your recorded start is replayed first. Building up speed before the timer
   starts is a trick the bot cannot do on its own, so it just copies yours.
2. The bot takes over and surfs the rest.
3. It gets a score for how far along the map it travelled, and a little of that
   score feeds back into how it steers next time.

When you surf, the only thing that really matters is the angle between the way
you are holding your keys and the way you are already moving. So that angle is
the one thing the bot decides, a few times per second. Everything else, the mouse
movement and which key is held, is worked out from that angle afterwards.

Before any of this, the bot copies a run of yours directly, so it starts out
already surfing rather than flailing around. Without that first step it learns to
twitch left and right very fast, which scores well but looks nothing like real
surfing.

## Teaching it with your own runs

This is the most useful thing you can do for it.

The bot currently has one run of yours to learn from. It has never seen what
happens when you are slightly off your usual line, which is why it can surf a
ramp nicely and then have no idea how to save itself.

More recordings fix that. They do not need to be fast or clean. A messy run where
you wobble and recover is worth more than another perfect one, because recovering
is the part it is missing.

See [HOWTO.md](HOWTO.md) for how to add them.

## What is in here

| folder | what it is |
|---|---|
| `plugin/` | the server plugin that drives the bot and records what happened |
| `tools/` | training, scoring, replay reading, reports |
| `web/` | the local replay viewer |
| `docs/` | notes on what was tried and what worked |

`docs/experiments.md` is a running log of every change and whether it helped,
including the ones that did not.

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
