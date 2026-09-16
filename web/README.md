# Local replay viewer

Watch the bot's runs as a live 3D scene, the way offstyles.net does it — a real
WebGL2 render of the actual map driven by a small `.replay` file, not a video.

```powershell
.\web\run.ps1          # starts backend + frontend, opens the browser
.\web\run.ps1 -Stop
```

- frontend `http://127.0.0.1:3000`
- backend `http://127.0.0.1:8787`

First run installs npm dependencies. The first load of a given map spends a few
seconds bz2-compressing its BSP; after that it is cached in `data/webcache/`.

## What this is

Two upstream repos, cloned and pointed at local data:

| | |
|---|---|
| [`offstyles-web/`](https://github.com/offstyles/offstyles-web) | the Vue site (records list, record page) |
| [`replay-viewer/`](https://github.com/offstyles/replay-viewer) | the viewer component — a vendored WebGL2 port of [noclip.website](https://github.com/magcius/noclip.website)'s Source engine renderer, plus a Rust→WASM replay parser |

Neither ships a backend. Upstream's Vite config proxies `/api` to the live
offstyles.net, whose API is closed and only knows *their* replays — our bot's runs
are not in it. So `tools/webserve.py` implements the endpoints the viewer needs
against local files:

| endpoint | serves |
|---|---|
| `POST /api/csspak/batch` | CS:S assets by Source path, read from the VPKs in your install |
| `GET /api/replay?id=` | a `.replay` from `data/csai/replays/` or shavit's replaybot dirs |
| `GET /api/times` | record list built from those files (with real sync/strafes/jumps) |
| `GET /maps/NAME.bsp.bz2` | the map from `cstrike/maps/`, bz2'd and cached |

## Where the replays come from

`csai_eval` records every tick of the best greedy run and writes a genuine shavit
v9 `.replay` (`plugin/include/csai_replay.inc`). It is the same format the timer
writes, verified by round-tripping through our own independent parser in
`tools/replay.py` — so the bot's runs and your human runs sit side by side in the
same viewer, on the same map.

```powershell
.\tools\eval.ps1 -Runs 3        # -> data/csai/replays/surf_demise_genN.replay
```

## Changes made to the upstream repos

Kept minimal and marked:

1. `offstyles-web/vite.config.ts` — proxy `/api` **and** `/maps` to the local
   backend instead of offstyles.net.
2. `offstyles-web/package.json` — `@offstyles/replay-viewer` points at the sibling
   clone (`file:../replay-viewer`) so local edits apply; `pako` and `lzma1` added
   because npm does not hoist a `file:`-linked package's own dependencies.
3. `replay-viewer/src/ReplayViewerOverlay.vue` — `fastdlBaseUrl` → `/maps`, so the
   BSP comes from your install rather than a public mirror (custom maps work
   offline).
4. `replay-viewer/src/ReplayViewerOverlay.vue` — `waitForNextPaint()` falls back
   to a macrotask when `document.visibilityState === "hidden"`. Upstream awaits
   two `requestAnimationFrame`s, which never fire in a background tab, so the
   whole load sequence deadlocks at "Initializing renderer..." if you switch away
   during the map download — precisely when you would.

## Licensing — read before publishing

**Both upstream repos are public but carry no LICENSE file**, which under default
copyright means all rights reserved. Cloning and running locally for yourself is
the ordinary case. **Hosting a copy publicly is not permitted without the
author's consent** — ask first.

Separately, the CS:S map and texture assets this serves are Valve's. A public
deployment would be redistributing them. For a personal/LAN tool this is moot; to
publish, ship geometry only or require viewers to supply their own game files.

The vendored noclip.website portion is MIT and fine with attribution; its notice
ships at `replay-viewer/src/noclip/LICENSE`.
