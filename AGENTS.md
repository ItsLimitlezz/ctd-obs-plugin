# AGENTS.md — ctd-obs-plugin
> Team rules live in the hub: github.com/ItsLimitlezz/CTD-Software-Suite → AGENTS.md (read it first). This file is repo-specific.

## What this is
**CTD OBS Plugin** (CMake project name `ctd-live-source`, display name "CTD Live Source", v0.1.0 in `buildspec.json`): an OBS Studio plugin that plays live SRT feeds from MediaMTX at the live edge, with a CTD Hub dock, issue detection/fixes, audio repair, delays, timecode and sync groups, and a CTD Relay dock that reports OBS state to CTD Relay.
Platforms: Windows x64, macOS (universal), Ubuntu 24.04 x86_64. Requires OBS 32.2+ on Windows/macOS. Public repo, GPL-2.0-or-later (open-source).

## Layout
| Path | What |
| --- | --- |
| `src/engine/` | Engine: C + FFmpeg only, no libobs (connect, demux, decode, live clock, audio norm, LTC, sync groups) |
| `src/obs/` | OBS source/audio-source, properties, issues, loss card, servers, webhook, obs-websocket vendor `ctd` |
| `src/ui/`, `src/multistream.c` | Qt docks (CTD Hub, CTD Relay) and the CTD Relay client |
| `src/api/` | MediaMTX API client and poller |
| `tools/ctd-live-probe/` | The engine on the command line (also `ltc-gen`) |
| `test/` | Acceptance harness (`scenarios.sh`, `run-local.ps1`, MediaMTX config, `check.py`) |
| `data/`, `assets/` | Locale, fonts (OFL), icons, `sync-clip.html` |
| `docs/api.md` | obs-websocket / webhook / Relay event API |
| `buildspec.json`, `CMakePresets.json`, `cmake/`, `build-aux/` | obs-plugintemplate build system |

## Build & test
Commands from README → Build (downloads OBS sources + deps into `.deps/` on first configure; CMake 3.28+):
- **Windows (PC)** — VS 2022: `cmake --preset windows-x64` · `cmake --build --preset windows-x64` · `cmake --install build_x64 --config RelWithDebInfo --prefix release`
- **macOS (Mac)** — Xcode 26.5: `cmake --preset macos` · `cmake --build --preset macos` · `cmake --install build_macos --config RelWithDebInfo --prefix release`
- **Ubuntu 24.04** — `cmake --preset ubuntu-x86_64 -DCMAKE_INSTALL_PREFIX=/usr` · `cmake --build --preset ubuntu-x86_64` · `cmake --install build_x86_64 --prefix release` (needs `ppa:obsproject/obs-studio` plus packages listed in README). Which machine does Linux test builds: TBD (owner to confirm; PC is Windows).
- **Probe:** add `-DENABLE_PROBE=ON` to configure, or `cmake -S tools/ctd-live-probe -B build-probe`.
- **Harness:** `bash test/scenarios.sh [T1 T2 T6 T11 ...]` (needs docker, curl, python3); `T6_MINUTES=3 bash test/scenarios.sh T6`. On Windows without Docker: `test/run-local.ps1`.
- Formatting: clang-format 19 (C/C++) and gersemi (CMake); CI fails on diffs.
- **Cloud can't build or run any of this.** Tag PRs `needs-build:pc` (Windows) or `needs-build:mac` (macOS).

## Release
Open-source (GPL-2.0-or-later) so prod builds may use GitHub Actions. Per README: push a version tag such as `0.2.0` to `main`; `push.yaml` builds all three platforms and opens a **draft** GitHub release with the packages and checksums. `dispatch.yaml` is the manual build. Release still needs the hub's Senior check first. macOS packages are currently unsigned (see README → Install; signing helper `tools/macos-signing.ps1`).

## Conventions & gotchas
- Workflows `pr-pull.yaml`, `push.yaml` (and `harness.yaml` for engine/tools/test changes) run on every PR and push to `main`, not only on tags/dispatch. This differs from the hub's build policy; TBD (owner to confirm whether to change).
- The engine in `src/engine/` must stay free of libobs and must not own threads (host runs `ctd_engine_run()`); see `src/engine/engine.h`.
- The plugin links the FFmpeg that OBS ships; never bundle FFmpeg (README, REQUIREMENTS F5).
- Always reads from MediaMTX over SRT; playback must never depend on the MediaMTX API.
- `REQUIREMENTS.md` is the v1 contract; `SPEC.md` adds to it and overrides it where they conflict.
- License is GPL-2.0-or-later because it links libobs/FFmpeg; brand fonts in `data/fonts` are SIL OFL 1.1.
- Pairs with CTD Relay (API v1.2+; v1.4 events, v1.6 remote-control) and CTD Overlay; changes to events in `docs/api.md` affect those repos: post on the hub's AGENT-REQUESTS.md.

## Required reading
- `README.md` (Build, Use, CTD Relay dock sections)
- `REQUIREMENTS.md` then `SPEC.md` (SPEC section 10 = test scenarios, section 11 = milestones)
- `docs/api.md`
- Lessons file: none yet.
