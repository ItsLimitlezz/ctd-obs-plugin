# CTD Live Source — OBS plugin requirements

An OBS source purpose-built to receive live SRT feeds from MediaMTX.
This document is the contract; code exists to satisfy it.

## 1. The problem it must solve

OBS's stock Media Source is a file player adapted to networks. Observed
failures with a live SRT feed, reproduced repeatedly in production testing:

- After any transient (a wi-fi blip upstream), it drops late **video** to stay
  current but queues late **audio** without bound → video "choppy but live",
  audio drifting 60s+ behind, permanently.
- It reports itself healthy the entire time (state PLAYING, clock advancing),
  so nothing external can detect the failure to trigger recovery.
- Recovery requires a human toggling the source.

The plugin's one-sentence mission: **play the live edge, forever, unattended.**

## 2. Functional requirements

### Connection
- F1. Connects to MediaMTX over SRT (caller mode) given: host, port,
  stream path. Builds the MediaMTX URL dialect itself
  (`streamid=read:<path>`, latency in the unit the transport layer expects).
  The user never sees or types a URL.
- F2. SRT latency budget is a setting (default 120 ms).
- F3. Reconnects automatically on: connection failure, EOF, demux error,
  read stall (no packet for N seconds, default 8), or settings change.
  Fixed short delay between attempts (default 2 s). Reconnection continues
  indefinitely — a live rig must come back when the stream does.
- F4. Never blocks OBS's UI or graphics thread. All network and decode work on
  a worker thread; teardown must interrupt any blocking I/O promptly
  (< ~250 ms) via the demuxer's interrupt callback.

### Media
- F5. Demuxes MPEG-TS from SRT; decodes H.264 video (HEVC if the decoder is
  present) and AAC audio using the FFmpeg libraries OBS ships. No bundled
  FFmpeg, no version fork.
- F6. Video delivered to OBS as async source frames in native format
  (I420/NV12 passthrough — no needless conversion), with correct color range.
- F7. Audio delivered in the decoder's native layout when OBS supports it
  (AAC → planar float), correct sample rate and speaker layout.
- F8. Streams with only one track (video-only / audio-only) still play.

### The clock policy (the heart of the plugin)
- F9. On (re)connect, the first frame's PTS is pinned to the wall clock; all
  subsequent frames are timed as `pts + offset`.
- F10. **Bounded drift, both tracks:** if a frame's mapped time falls behind
  the wall clock by more than the drift cap (default 700 ms), re-base the
  offset so playback snaps to the live edge. Audio and video share one
  offset, so A/V skew can never exceed the cap.
- F11. No elastic buffering anywhere in the plugin. The only intentional
  buffer is SRT's own latency window. Memory use is O(1) with time.
- F12. Backwards PTS jumps (stream restart, camera clock reset) are treated
  exactly like drift: snap and continue, don't stall.

### Health & observability
- F13. Counters visible in the source's properties: video frames, audio
  frames, live-edge resyncs, reconnects.
- F14. Meaningful events logged to the OBS log (connect with codec names,
  reconnect with reason, resync) — enough to diagnose a show afterwards.
- F15. "Looks healthy" must mean *is* healthy: if frames are flowing, they
  are at most drift-cap from live. There is no state where the plugin plays
  content minutes behind.

### Settings UI
- F16. Fields: server address, SRT port, stream path, SRT latency (ms), drift
  cap (ms), reconnect delay (s), stall threshold (s). Sliders for the time
  values with sane ranges; defaults match MediaMTX's defaults
  (port 8890, latency 120 ms).
- F17. Defaults must produce a working setup with zero tuning when pointed at
  a MediaMTX server on the LAN.

## 3. Non-functional requirements

- N1. Steady-state added latency (SRT ingress → OBS compositor) ≤ ~250 ms
  with defaults on a LAN.
- N2. One 1080p30 H.264 + AAC stream decodes in software comfortably on a
  midrange PC (hardware decode is v2).
- N3. Cannot crash OBS on: malformed streams, mid-stream resolution changes,
  server restarts, or rapid settings changes. Every FFmpeg return code
  handled; teardown is leak-free (verified by repeated connect/destroy).
- N4. GPL-2.0 licensed (required for linking against OBS/FFmpeg in an OBS
  plugin) — acceptable because it ships as a separate repo/binary from the
  MIT-licensed app.

## 4. Failure-mode contract

| Event | Required behaviour |
| --- | --- |
| Server unreachable | retry every reconnect-delay, forever, quietly |
| Stream not publishing yet | same as unreachable; starts when it appears |
| Publisher drops mid-show | stall detected ≤ stall-threshold, reconnect loop until it returns |
| Wi-fi transient upstream | ≤ drift-cap of degradation, then hard snap to live; **no accumulation** |
| Server restarted (config change) | reconnect picks it up automatically |
| Stream restarted (PTS reset) | snap to live via F12, no manual action |
| OBS scene switch / source hidden | keeps running (live feeds must not miss a beat) |
| Settings changed | clean teardown, immediate reconnect with new settings |

## 5. Explicitly out of scope for v1

- Hardware decode (v2; software is sufficient for one stream)
- Multiple streams per source instance (add more sources instead)
- Protocols other than SRT (RTSP/RTMP stay on the stock source)
- Recording/buffered rewind (antithetical to the live-edge mission)
- Audio resampling beyond what OBS accepts natively

## 6. Acceptance tests (definition of "works well")

1. **Cold start:** add source, enter host/path, publisher already live →
   picture + audio within 3 s.
2. **Waiting start:** add source before the publisher exists → plays within
   3 s of the stream appearing, no interaction.
3. **90-minute soak** with a real camera on wi-fi: A/V stays within the drift
   cap for the entire run; zero manual interventions; memory flat.
4. **Transient injection:** interrupt the camera's network for 2–5 s,
   repeatedly → each time, playback resumes at the live edge within
   drift-cap + reconnect time; audio never lags more than the cap.
5. **Server bounce:** restart MediaMTX mid-play → source recovers
   unattended.
6. **Torture:** toggle settings rapidly, hide/show the source, duplicate the
   scene → no crash, no leak, no orphaned threads.

## 7. Build & delivery plan

Reality: OBS on the production machine is Windows; development happens on
a Mac. The plugin cannot be compile-tested for Windows locally.

1. Source lives in this repo (`ctd-obs-plugin`).
2. Windows, macOS and Ubuntu binaries come from GitHub Actions (the
   obs-plugintemplate workflows in `.github/workflows/`, SPEC.md B1).
3. macOS build is compiled and smoke-tested locally against OBS-on-Mac first —
   same code, same FFmpeg APIs — so what reaches Windows CI is known-good
   logic, not a first draft.
4. Acceptance tests 1–5 run on the production PC against the real rig.
