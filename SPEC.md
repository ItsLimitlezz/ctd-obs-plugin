# CTD OBS Plugin: product spec

**What it is:** the plugin anyone running OBS seriously installs to bring live feeds in. It connects OBS to a
[MediaMTX](https://github.com/bluenviron/mediamtx) server, shows every stream on that server in one panel, adds any of
them to a scene in one click, plays them at the live edge for hours without anyone touching it, says plainly what is
wrong when a feed goes bad (and fixes it where it can), repairs broken audio, and keeps several feeds in sync using
timecode.

**What it replaces:** OBS's stock Media Source pointed at an `srt://` URL, a hand-typed URL per camera, guessing why
a feed is choppy, the "Video Delay (Async)" filter plus audio Sync Offset tuned by ear, and toggling sources mid-show.

`REQUIREMENTS.md` is the v1 contract and still applies. This document adds to it and overrides it where they conflict.

---

## Prompt for Claude Code

> Read `REQUIREMENTS.md`, `README.md`, `src/ctd-live-source.c`, then this file, `SPEC.md`.
> Build the plugin milestone by milestone, in the order in section 11, one PR per numbered step. Every PR must
> build on Windows, macOS and Linux in CI and pass the test scenarios (section 10) that apply to it.
> Don't rewrite what v1 already does well (section 2).
> Where a decision isn't covered here, pick the option that keeps playback at the live edge with no human intervention
> and is simplest for someone who has never seen MediaMTX, and write the decision down in the PR description.

---

## 1. Who uses it and how

**Anyone:** a streamer, church, club, event or IRL crew pushing phones, encoders, action cams or bonded units into
MediaMTX (over SRT, RTMP, RTSP or WebRTC; MediaMTX converts), with OBS pulling those feeds over SRT. One to sixteen
feeds per OBS machine. MediaMTX on the same machine, on the LAN, or in the cloud.

**The reference show (Miami or Bust, April 2027):** ATEM in a car → bonded 5G/Starlink encoder → SRT → MediaMTX
(cloud) → OBS + this plugin → 16:9 and 9:16 outputs. Nobody sits at the OBS machine; the director is in another car.
Warm-ups until then: iPhone (Blackmagic Camera app) → SRT → MediaMTX → OBS.

The plugin **always reads from MediaMTX over SRT**. The unreliable link (camera to MediaMTX) is upstream of the plugin.
The plugin's job is to turn whatever MediaMTX hands it into a steady, live-edge, 48 kHz feed, to tell the operator
what is happening upstream, and to keep feeds aligned with each other.

## 2. Keep from v1 (already right)

- Worker thread per source, interruptible I/O, clean teardown (F4, N3).
- Endless reconnect loop with stall detection (F3).
- One shared live clock for audio and video, with a hard snap when lateness passes the drift cap (F9 to F12).
- No elastic buffering (F11). Delays added on purpose (section 8) are fixed, bounded and visible, never grown by the
  network.
- The MediaMTX URL is built for the user (`streamid=read:<path>`).
- GPL-2.0-or-later.

## 3. Pieces

| Piece | What it is |
|---|---|
| **CTD Live Source (SRT)** | The OBS source. One per feed. Add as many as you want to any scene. Name stays as in v1 so existing scenes keep working. |
| **CTD Hub** | An OBS dock (Docks menu). Lists the server's streams, adds them to scenes, shows issues and sync groups. |
| **Server profiles** | Saved MediaMTX connections (address, ports, API, credentials). Sources point at a profile + path, never a raw URL. |
| **Engine** | The connect/demux/decode/clock/audio/timecode core, with no libobs dependency (section 10). |
| **ctd-live-probe** | A command-line tool built from the engine, used by CI and for debugging without OBS. |
| **Control API** | obs-websocket vendor requests and events, plus an optional webhook (section 9). This is how CTD Live Studio, CTD Live Mobile and anyone else drive and read the plugin. |

---

## 4. Milestone 1: the source is rock solid

These are the fixes found in v1 and the behaviour a live show needs. Requirement numbers R1 to R20 are kept from
the earlier v2 spec so existing notes stay valid.

### 4.1 Problems in v1

| # | Problem | Evidence |
|---|---|---|
| P1 | **Video timestamps come from the packet, not the decoded frame.** `run_session` stamps every frame from `avcodec_receive_frame` with the PTS of the packet just sent. With B-frames or frame-threaded decode delay, frames get wrong, sometimes non-monotonic, timestamps. OBS then drops or repeats frames: choppy video. | `src/ctd-live-source.c`, lines 281 to 305. |
| P2 | **The source goes transparent on signal loss.** `obs_source_output_video(ctx->source, NULL)` exposes whatever is below it in the scene. | Code review. |
| P3 | **Slow clock drift is handled only by hard snaps.** Sender and OBS clocks drift a few hundred ms over hours, ending in a visible and audible snap. | Design gap. |
| P4 | **Audio format changes mid-stream are not handled.** A Bluetooth mic on the iPhone dropped capture to 16 or 24 kHz and the audio turned choppy and drifted (Oct 7, 2026 warm-up). | Field report. |
| P5 | **Garbage video after (re)connect or loss.** Decoding starts on a non-keyframe and the decoder is not flushed on discontinuities. | Code review. |
| P6 | **Software decode only.** Several feeds on one machine need hardware decode. | REQUIREMENTS.md section 5. |

### 4.2 Correct timing
- **R1.** Stamp video with `frame->best_effort_timestamp`, falling back to `frame->pts`, then packet DTS. Same for
  audio. Never assume one output frame per input packet.
- **R2.** A discontinuity is a PTS jump of more than 1 s forward or any jump backward, a codec parameter change, or a
  MediaMTX `alwaysAvailable` slate switch. On one: flush both decoders (`avcodec_flush_buffers`), re-establish the
  live clock (v1 F12), and wait for a keyframe (R3). No reconnect.
- **R3.** Keyframe gating: after connect, reconnect or a discontinuity, drop video packets until one flagged
  `AV_PKT_FLAG_KEY`. Audio may play immediately.

### 4.3 Live clock with soft correction
- **R4.** Keep v1's hard snap at the drift cap as the safety net.
- **R5.** Soft correction inside the cap. Lateness = `now - (pts + offset)`, as a ~2 s moving average. The lateness
  just after (re)connect is the **target**. While the average stays within ±(drift cap / 2) of the target, steer back:
  audio with `swr_set_compensation` up to ±0.5% (inaudible); video by re-basing the offset in steps of at most 2 ms
  per second. A hard snap should then only happen after a real network event.
- **R6.** Current lateness, target and compensation ratio appear in the stats.

### 4.4 Audio normalization (always on)
- **R7.** Always hand OBS **48 kHz, stereo, float planar**, whatever arrives (8 to 96 kHz, mono or stereo, any sample
  format AAC, Opus or MP3 decoders produce), via libswresample (OBS ships it).
- **R8.** Rebuild the resampler on the fly when rate, layout or format changes: no reconnect, no gap longer than one
  audio frame, and a log line (`[ctd-live:Cam 1] audio format changed 48000 Hz stereo → 16000 Hz mono`).
- **R9.** Mono becomes dual mono (centre), not left only.

### 4.5 Signal loss
- **R10.** State machine: `CONNECTING → LIVE → STALLED → RECONNECTING → (LIVE | NO_SIGNAL)`.
  STALLED = no media for more than **hold time** (default 1500 ms). NO_SIGNAL = still nothing after hold time plus
  the stall threshold.
- **R11.** **On signal loss** setting: *Hold last frame, then slate* (default; slate is a picture or video file, or a
  built-in navy `#0D0B1E` frame reading "SIGNAL LOST · RECONNECTING" with the source name), *Hold last frame*, or
  *Transparent* (v1 behaviour, so a scene can put its own slate underneath).
  - A slate picture or video is fitted to the feed's picture size keeping its shape (black bars where the shapes
    differ), so the scene layout doesn't move.
  - A video slate (MP4, M4V, MOV, MKV or WebM) plays from its start each time the signal is lost and loops until the
    feed returns. Its sound is never played (R12). It is decoded only while the source is on screen.
  - A slate file that can't be read falls back to the built-in slate, logged once.
- **R12.** No audio while STALLED or NO_SIGNAL. Never loop the last buffer.
- **R13.** On return: keyframe (R3), new clock, cut straight to live.

### 4.6 Observability and controls
- **R16.** Stats in properties: state, lateness/target, compensation, input (resolution, codec, fps, audio rate and
  channels), resyncs, reconnects, upstream RTT/loss/bitrate (from the hub poller, section 5).
- **R17.** **Reconnect now** button.
- **R18.** Every log line prefixed `[ctd-live:<source name>]`.

### 4.7 Hardware decode
- **R19.** **Decoder:** Auto (default), Software, Hardware. Auto tries `d3d12va` then `dxva2` (Windows),
  `videotoolbox` (macOS), `vaapi` or `cuda` (Linux), transfers to system memory as NV12, and falls back to software on
  any failure, logged once. Not `d3d11va`: copying its frames back to system memory costs more CPU than software
  decoding (measured: 4 × 1080p30 on an RTX 3060, 18% of the machine against 10.5% in software and about 5.5% with
  `d3d12va` or `dxva2`).
- **R20.** Hardware decode still satisfies R1 to R3.

### 4.8 Source settings

| Setting | Default | Notes |
|---|---|---|
| Server | the default profile | section 5 |
| Stream | none | dropdown of the server's paths, live ones first; free text allowed for paths that aren't live yet |
| SRT latency | Auto | Auto = max(120 ms, 4 × RTT of the local hop), recalculated on reconnect. Manual 20 to 8000 ms |
| Max drift before snapping to live | 700 ms | R4 |
| Soft correction | on | R5 |
| Reconnect delay | 1 s | |
| Reconnect after stalled for | 3 s | |
| Hold last frame for | 1500 ms | R10 |
| On signal loss | Hold, then slate | R11 |
| Slate picture or video | built-in | R11 |
| Decoder | Auto | R19 |
| Audio repair | section 7 | |
| Delay and sync | section 8 | |

---

## 5. Milestone 2: the hub (MediaMTX sync, many feeds)

### 5.1 Server profiles
- **H1.** Profiles are stored in the plugin's own config (`obs_module_config_path`), not in the scene collection, so
  every scene collection shares them. Fields: name, host, SRT port (8890), API URL (`http://<host>:9997`, blank to
  disable), API username/password or bearer token, SRT read username/password, SRT passphrase.
- **H2.** Credentials go into the SRT streamid the way MediaMTX expects (`read:<path>:<user>:<pass>`) and the
  passphrase into the SRT options. Secrets are never written to the OBS log or the stats text.
- **H3.** One profile is the default. A first run with no profiles offers `127.0.0.1` and tests it.
- **H4.** Sources store `profile id + path`. Changing a profile's address moves every source that uses it, with a
  clean reconnect each.

### 5.2 Syncing with MediaMTX
- **H5.** One poller per profile (not per source), on its own thread, every 2 s (setting 1 to 10 s). It reads:
  - `GET /v3/paths/list` (all pages): name, whether a publisher is present, publisher type, tracks (codecs),
    bytes received (gives bitrate), readers.
  - `GET /v3/srtconns/list` (and the RTMP, RTSP and WebRTC session lists when present) for the publisher of each
    path: RTT, packet loss, retransmits, drops, receive rate, link capacity, remote address.
  - The plugin's own read connections in `srtconns`, to measure the local hop separately from the upstream link.
- **H6.** Handle differences between MediaMTX versions (field names, missing endpoints) without crashing or
  spamming the log. Test against the latest release and the version CTD Live Server pins. Document the minimum
  supported version in the README.
- **H7.** The API being down, slow or unauthorised never affects playback. It shows as a profile status
  ("API unreachable", "API login failed") in the hub.
- **H8.** **Optional config writes**, off unless "Allow CTD Hub to change MediaMTX" is ticked on the profile:
  create a path, remove a path, and **kick a publisher** (`POST /v3/<protocol>conns/kick/<id>`) to force a stuck
  camera to reconnect. Every write asks for confirmation in the UI and is logged.

### 5.3 The dock
- **H9.** **CTD Hub** dock, built in Qt with the OBS frontend API, styled to match OBS's theme. Top: profile picker
  and its status (connected, MediaMTX version, API latency).
- **H10.** **Streams list**, one row per path: live dot, name, publisher protocol and remote address, resolution,
  codec, fps, bitrate sparkline, RTT and loss (SRT publishers), an issues badge (section 6.1), and an "in OBS" badge
  naming the scenes that use it. Live streams first, then idle paths.
- **H11.** Row actions: **Add to current scene** (creates a CTD Live Source bound to the path, named after it, sized
  to the canvas), **Add to scene ▸**, **Copy publish URL** (SRT, RTMP, WHIP, as MediaMTX would accept them for this
  path, with credentials if set), **Show QR code** of the chosen publish URL (for camera apps that scan one),
  **Reconnect**, **Kick publisher** (H8), **Open properties**.
- **H12.** **Add all live streams** to the current scene, optionally arranged as a grid (2x2, 3x3, 4x4) for a
  multiview scene.
- **H13.** **Auto-add new streams** (off by default): when a new path goes live, add a source for it to a chosen
  scene. Never removes sources on its own.
- **H14.** A source whose path no longer exists on the server shows "Path not on server" in the dock and its stats,
  and keeps retrying.

### 5.4 Many feeds
- **H15.** Each source stays independent (own thread, own decoder), but shares its profile's poller.
- **H16.** Targets, measured and written down in the README: 8 × 1080p30 feeds on a mid-range PC with hardware
  decode; 4 × 1080p30 in software on an 8-core CPU. No feed may degrade another (one stalling feed never delays
  the rest).
- **H17.** Sources keep running when hidden or not in the current scene (v1 failure table), with an opt-in per
  source "Disconnect when not in any active scene" to save bandwidth.

### 5.5 CTD Multi-Stream (the hub for everything after OBS)
CTD Multi-Stream restreams the show to several platforms and is the one service the plugin connects to for
everything after OBS: destinations, chat, and the overlays of other CTD apps (CTD Overlay registers its overlays
with Multi-Stream, so the plugin never needs CTD Overlay's address). Contract: `docs/API.md` of
`ItsLimitlezz/ctd-multi-stream`, API v1.2 (profiles, hub, pairing; still being built). Until this ships,
Multi-Stream's own dock covers status and chat: **Settings → API → OBS dock** gives a link for OBS's **Docks →
Custom Browser Docks**.
- **H18.** **Connection by pairing**, no token copying: the plugin calls `POST /api/v1/pairing` (`kind:
  "ctd-obs-plugin"`, scopes `status:read chat:read ingest:read apps:read apps:write`), shows the code, and polls
  `POST /pairing/{id}/token` every `interval` s until the user approves it in Multi-Stream's **Settings → API**
  (200: token, returned once), denies it (403) or it expires (410). Base URL `http://<host>:30080/api/v1` on the LAN
  (HTTPS on 30443 also works). The token is stored in the plugin's own config like profile secrets (H1, H2), never
  logged. Pasting a token by hand stays possible.
- **H19.** **Profile:** "this OBS goes to profile X". `GET /profiles` lists the set-ups (e.g. "Miami or Bust",
  "DPRTMNT") with colour, live status and counts; profiles can be live at once. The choice is per OBS (plugin
  config); a token limited to one profile skips the choice. Everything below is for the chosen profile
  (`?profile=<id>` on `/status`, `/chat`, `/events/poll`).
- **H20.** **Destinations: route OBS to the profile.** `GET /profiles/{id}/ingest` gives the server URL and a stream
  key per feed. On the user's say-so (a button, with the change shown first), the plugin sets OBS's stream service
  through the frontend API: landscape on the main output, vertical on Aitum Vertical when it is installed. Stream
  keys are secrets: never logged or shown in full. Never changes the output while streaming.
- **H21.** **Overlays: pull them in.** `GET /profiles/{id}/overlays` lists every overlay assigned to the profile
  (from CTD Overlay or any other app): name, URL, size, `canvas` (landscape or vertical, else width > height),
  whether its app is online. The dock lists them with **Add to scene** (a browser source at the overlay's size, on
  the right canvas) and **Switch existing** (point browser sources already showing one of this app's overlays at the new
  profile's overlay with the same `meta.slot`, a role key that is the same for the equivalent overlay in every profile,
  and the same `canvas`; by name only when `slot` is missing), so a profile switch carries the scenes over. Never deletes sources.
- **H22.** **Talk to other apps through the hub.** Register as `ctd-obs-plugin-<machine>` (`PUT /apps/{id}`,
  re-sent at least every 60 s) and send requests such as `overlay.create` to CTD Overlay with `POST /messages`;
  replies arrive as `app.message` events. The registration offers the plugin's own capabilities (to be defined,
  e.g. which feeds are live in OBS).
- **H23.** **Status panel** in the CTD Hub dock for the chosen profile: the session (status, title, a TEST badge in
  test mode), each feed (landscape, vertical) with its bitrate, and one row per destination: platform, name, live
  or **down** (red, with the server's `message` as the reason), bitrate, fps, viewers and recoveries. One poller on
  its own thread reads `GET /status?profile=` every 2 s.
- **H24.** **Merged chat** in the dock, oldest first with each message's platform, following
  `GET /events/poll?profile=&after=<id>&types=destination.*,chat.*,app.*,profile.*` with a saved cursor. It honours
  `chat.message.deleted`, `chat.user.cleared` and `chat.cleared`. On `reset` (cursor too old, or the server
  restarted) it reloads `/status` and `/chat`.
- **H25.** A `destination.down` edge is logged and raised as an issue (section 6) with the server's reason;
  `destination.up` clears it.
- **H26.** Multi-Stream being unreachable, slow, or rejecting the token never affects playback (as H7). Its errors
  (RFC 9457 problem+json) show as the panel's status, and the poller stays well under the 600 requests a minute a
  token allows.

---

## 6. Milestone 3: issue detection and fixes

### 6.1 The issue engine
- **D1.** Each source and profile runs a small rules engine over its stats and the poller data. An issue has: id,
  severity (info, warning, critical), source, first seen, last seen, a plain-language title, the likely cause, and
  zero or more **fix actions**.
- **D2.** Issues appear in the CTD Hub issues panel (newest first), as a badge on the stream row, in the source's
  stats, in the OBS log (raised and cleared), and through the control API (section 9). An issue clears itself after
  its condition has been gone for 10 s.
- **D3.** No alert storms: one issue per (source, rule) at a time; flapping conditions are held for 10 s before
  clearing.

### 6.2 Rules (first set)

| Issue | Detected when | Fix actions |
|---|---|---|
| Camera offline | path has no publisher | Show publish URL / QR |
| Upstream packet loss | publisher SRT loss over 2% for 10 s | Raise the camera's SRT latency (suggest a value from RTT: at least 4 × RTT), lower camera bitrate |
| Upstream link slow | RTT over 800 ms, or bitrate above 80% of link capacity | Lower camera bitrate |
| Bitrate collapsed | bitrate under 30% of its 60 s average for 5 s | Informational |
| Local hop loss | the plugin's own read connection shows loss | Raise this source's SRT latency (one click) |
| Audio sample rate not 48 kHz / changed mid-stream | from the decoder | Explains the likely cause (Bluetooth mic on a phone drops to 16 or 24 kHz); normalization already handles it (section 4.4) |
| Audio missing | stream has no audio track, or audio stopped while video continues for 3 s | Informational |
| Audio silent | peak below −60 dBFS for 10 s while connected | Check the mic |
| Audio clipping | peaks at 0 dBFS more than 1% of the time over 10 s | Informational (gain is upstream) |
| One channel dead | one channel more than 40 dB below the other for 10 s | **Use the live channel on both sides** (section 7) |
| Timecode on a program channel | LTC detected on an audio channel that isn't marked as timecode | **Use that channel as timecode** (section 8) |
| Frozen picture | the decoded picture hasn't changed for 5 s (cheap checksum over a subsampled luma grid) while frames keep arriving | Kick publisher (H8) |
| Low frame rate | received fps under 80% of the stream's nominal fps for 10 s | Informational |
| Decoder falling behind | decode time per frame above the frame interval for 5 s | **Switch to hardware decode** |
| Frequent snaps | more than 3 hard snaps in 5 minutes | Raise this source's SRT latency or drift cap |
| A/V drift | soft correction pinned at its limit for 30 s | Informational (sender clock is far off) |
| Sync lost | a sync-group member lost timecode or exceeded the group's max delay | section 8 |

- **D4.** A fix action is one click in the hub and one vendor request in the API. Actions that change settings say
  what they change and can be undone from the same issue entry.
- **D5.** **Auto-fix** (per source, off by default): apply the safe fixes automatically (hardware decode, local SRT
  latency, dead-channel repair) and log what was done.

---

## 7. Milestone 4: audio repair

Section 4.4 makes every feed 48 kHz stereo. Audio repair is for feeds that are wrong in other ways, set per source
in properties and the hub.

- **A1.** **Channels:** Stereo (as is), Left on both, Right on both, Mono mix, Swap left and right, Invert right
  channel polarity.
- **A2.** **Fill gaps:** when audio packets are missing (a hole in audio timestamps longer than 5 ms), insert silence
  of exactly the missing length with 5 ms fades at the edges, instead of butting the next audio up against the last
  (which shifts sync and clicks). On by default; this is the main cure for "choppy" network audio.
- **A3.** **Lock audio to video:** soft correction (R5) steers audio toward video's clock instead of the shared
  target, for senders whose audio clock wanders. Off by default.
- **A4.** **Sample rate override:** treat incoming audio as a given rate, for senders that label 44.1 kHz audio as
  48 kHz (pitch is wrong). Default: as labelled.
- **A5.** **Reset audio** button: tear down and rebuild the audio decoder and resampler without touching video.
- **A6.** **Audio only / Video only** switches, so a feed can provide just one of the two.
- **A7.** Out of scope: noise suppression, EQ, compression and gain. OBS's own filters do those; the plugin's job is
  to deliver clean, correctly timed audio to them.

---

## 8. Milestone 5: delay and timecode sync

### 8.1 Manual delay (one feed)
- **S1.** **Delay video by** and **Delay audio by**, 0 to 2000 ms each, per source. Delaying audio fixes "lips after
  sound"; delaying video fixes "sound after lips". Changes apply smoothly (audio via compensation, video by
  repeating or dropping single frames), never as a jump while live.
- **S2.** Delays are implemented inside the plugin with its own bounded queue. Do **not** rely on OBS's async frame
  queue for delay: it holds a limited number of frames and resets on large timestamp gaps. Frames are released to
  OBS just ahead of their presentation time. A memory cap per source (default 512 MB) is enforced and the delay
  setting is limited to what fits at the current resolution, with a warning.
- **S3.** This replaces stacking OBS's "Video Delay (Async)" filter and audio Sync Offset on a feed: one place, both
  directions, shown in the stats and the API, and used by the sync groups below.

### 8.2 Timecode
- **S4.** **Timecode input** per source: None (default), LTC on left, LTC on right, LTC on channel N, or H.264/HEVC
  SEI timecode when the stream carries it. When LTC is chosen, that channel is **removed from program audio** (the
  other channel is used on both sides), because LTC is a loud tone.
- **S5.** LTC decoding runs on the decoded audio **before** resampling, at the original rate. Supports 23.976, 24,
  25, 29.97 drop and non-drop, and 30 fps, detected automatically. Reports the current timecode, rate, drop-frame
  flag, user bits, signal level and decode error rate. Prefer a small in-house biphase-mark decoder; libltc (LGPL-3.0)
  may be used only after confirming the license combination in the PR.
- **S6.** Each decoded LTC frame is pinned to the stream PTS of the sample where its sync word ends, giving a
  running map from stream time to timecode with sub-millisecond precision.
- **S7.** Auto-detect: if LTC shows up on any channel of a source with timecode set to None, raise the "Timecode on a
  program channel" issue (section 6.2) with a one-click fix.
- **S8.** LTC must survive the camera's AAC encode. Test at 64, 128 and 256 kbps.

### 8.3 Sync groups
- **S9.** **Sync group** per source (a name; empty = none). All sources in a group with working timecode are aligned
  so that the same timecode is on screen and in the mix at the same moment.
- **S10.** **Align to slowest feed** (default): measure each member's timecode at the same OBS instant; the member
  showing the earliest timecode is the reference and gets no added delay; every other member gets a delay equal to how
  far ahead it is. **Fixed latency** (option): if the timecode is time of day and the OBS machine's clock is NTP
  synced, present every member at `time of day − N ms`, which also shows each feed's true end-to-end delay.
- **S11.** Measurement uses a median over 5 s. Initial lock may jump (with a 20 ms audio fade). After lock, adjust
  only when the error exceeds half a frame for 2 s, and then ramp (audio compensation, single video frame repeats or
  drops), never jump on air.
- **S12.** **Max group delay** (default 1000 ms, up to 5000 ms, bounded by S2's memory cap). A member that would need
  more is held at the max and raises "Sync lost".
- **S13.** A member that loses timecode **freewheels** on its last delay and raises "Sync lost"; it relocks when
  timecode returns. A member that reconnects keeps its previous delay as the starting guess.
- **S14.** Manual delays (S1) add on top of the group delay, so a known in-camera A/V offset can still be trimmed.
- **S15.** The hub shows each group: members, timecode per member, measured offset, applied delay, lock state.

### 8.4 Sync check (optional, after the above)
- **S16.** A **sync check** for one feed: the user plays the plugin's sync clip (white flash and 1 kHz beep once a
  second, the same signal the test harness uses; downloadable from the hub) on a phone or tablet in front of the
  camera. The plugin measures the A/V offset in that feed over 10 s and offers to set S1's delays.

---

## 9. Control API and integrations

- **I1.** Register an **obs-websocket vendor** named `ctd` (obs-websocket 5 vendor API). Anything that already talks
  to OBS over WebSocket can then use the plugin with no extra port or password.
- **I2.** Requests (each milestone adds its own): `GetServers`, `GetStreams`, `GetFeeds`, `GetFeed`,
  `AddFeedToScene`, `ReconnectFeed`, `KickPublisher`, `GetIssues`, `ApplyFix`, `SetAudioRepair`, `SetDelays`,
  `GetSyncGroups`, `SetSyncGroup`, `GetPublishUrl`.
- **I3.** Events: `FeedStateChanged` (CONNECTING, LIVE, STALLED, RECONNECTING, NO_SIGNAL), `IssueRaised`,
  `IssueCleared`, `SyncStatusChanged`, `StreamAppeared`, `StreamGone`.
- **I4.** **Webhook** (optional, per profile): POST the same events as JSON with a 1 s timeout from a background
  thread, for things that don't speak OBS WebSocket (for example a graphics server showing "signal lost").
  Fire and forget; a failing webhook never affects playback.
- **I5.** Document every request and event with an example in `docs/api.md`.

**Later, not specified here (the API above is what makes them possible):**
- **CTD Live Mobile:** health badges and signal-lost alerts on its multiview tiles, one-tap fixes, sync status.
- **CTD Live Studio:** reconnect, kick and resync mapped to controller buttons; feed health in the director's view.
- Tally back to camera operators, and CTD Live Mobile as a publisher that gets feedback (lower your bitrate).

---

## 10. Architecture, build and tests

### 10.1 Code layout
- **E1.** `src/engine/` (C, FFmpeg only, **no libobs**): connect, demux, decode, clock, audio normalization and
  repair, delay queue, LTC decoder, issue rules, state machine. It reports out through callbacks
  (`on_video`, `on_audio`, `on_state`, `on_stats`, `on_issue`, `on_timecode`).
- **E2.** `src/api/` (C): the MediaMTX API client and poller. Use libcurl if it can be linked on all three platforms
  without bundling trouble; otherwise a minimal HTTP/1.1 client over sockets is fine (MediaMTX's API is plain JSON
  over HTTP). JSON via a small vendored parser (cJSON or similar, MIT).
- **E3.** `src/obs/` (C): the OBS source, properties, config storage, obs-websocket vendor, webhook.
- **E4.** `src/ui/` (C++, Qt6): the CTD Hub dock.
- **E5.** `tools/ctd-live-probe/`: the CLI on top of the engine: `ctd-live-probe --host --port --path
  [--seconds N] [--json]`. Prints state, lateness, fps, audio rate, compensation, drops, resyncs, timecode, issues
  and measured A/V offset every second; non-zero exit if an acceptance bound is broken.

### 10.2 Build
- **B1.** Move to the official **obs-plugintemplate** layout (CMake, Qt6, frontend API), with its GitHub Actions
  building Windows x64, macOS universal and Ubuntu 24.04 x64. This replaces the current Windows workflow (which
  synthesises import libraries from OBS DLLs and cannot build Qt UI). Keep linking against the FFmpeg OBS ships.
- **B2.** Releases: a Windows installer, a signed and notarized macOS `.pkg`, and a Linux `.deb`, attached to each
  GitHub release. The binary module name stays `ctd-live-source` so existing installs upgrade in place.

### 10.3 Non-functional
- **N1.** Added latency from MediaMTX to the OBS compositor ≤ 300 ms with defaults on a LAN (no delay or sync set).
- **N2.** 10-hour run with 4 feeds: memory flat (±5 MB per feed, excluding configured delay), no thread or handle
  growth, CPU flat.
- **N3.** Never crashes OBS on malformed or truncated MPEG-TS, codec, resolution or sample-rate changes, rapid
  settings changes, the server or API disappearing mid-read, or OBS closing mid-reconnect. Every FFmpeg return code
  is checked.
- **N4.** A/V sync within ±40 ms at all times outside a snap; sync-group members within ±1 frame of each other once
  locked.

### 10.4 Test harness
`test/` with Docker Compose:
- `mediamtx` (latest, API on; a second service on the pinned older version for H6).
- `publisher` × up to 4: FFmpeg sending SRT, 1080p30 H.264 **with B-frames** (`-bf 2`) and AAC, carrying the test
  signal (white flash and 1 kHz beep once a second, same timestamp) and, on one channel when asked, LTC generated
  from the same clock (publishers can be offset from each other by a set amount to test sync).
- `impair`: `tc netem` on each publisher's link.
- `probe`: `ctd-live-probe` writing JSON results.

| # | Scenario | Pass condition |
|---|---|---|
| T1 | Cold start, publisher already live | LIVE within 3 s |
| T2 | Probe started before the publisher | LIVE within 3 s of the publisher starting |
| T3 | B-frames on | Video timestamps strictly increasing, no duplicates or reordering (R1) |
| T4 | 2%, 5%, 10% loss upstream | Stays LIVE or recovers; lateness never above the drift cap; A/V within ±40 ms outside snaps; "Upstream packet loss" raised and cleared |
| T5 | 150 ms jitter | As T4 |
| T6 | 3 s outage every 60 s for 15 min | Each time: STALLED → NO_SIGNAL (if long enough) → LIVE within 3 s; no accumulation |
| T7 | Publisher restart (PTS reset) | Recovers via R2 without reconnecting |
| T8 | Sample rate switch 48 kHz → 16 kHz mono → 48 kHz stereo | Continuous 48 kHz output, resampler rebuilt and logged, A/V within ±40 ms (the Oct 7 bug) |
| T9 | Resolution switch 1080p → 720p → 1080p | Plays through, no reconnect |
| T10 | MediaMTX `alwaysAvailable` slate, publisher drops and returns | Seamless (R2) |
| T11 | MediaMTX restarted | Recovers within reconnect delay + 3 s; API status recovers |
| T12 | Sender clock 0.1% fast for 30 min | No hard snaps; soft correction holds the target (R5) |
| T13 | 10-hour soak, 4 feeds, 2% loss | N2 and N4 hold |
| T14 | API: paths appear and disappear, API down, wrong password, older MediaMTX version | Streams list correct within 2 poll intervals; playback unaffected; clear profile status |
| T15 | Audio gaps: drop 5% of audio packets only | Gaps filled with silence of the right length; A/V within ±40 ms (A2) |
| T16 | One dead channel, LTC on a program channel, frozen picture, silent audio | Each raises the right issue; each fix action works |
| T17 | LTC at 64, 128, 256 kbps AAC, all supported frame rates | Timecode decoded with under 0.1% frame errors |
| T18 | Sync group: 3 publishers offset by 0, 180 and 420 ms | Lock within 10 s; members within ±1 frame; no jumps after lock |
| T19 | Sync group member loses LTC, then the link, then both return | Freewheels, raises "Sync lost", relocks without a jump |
| T20 | Manual delays 0 → 1500 ms video, 0 → 800 ms audio, changed while live | Smooth change; memory within cap |

**By hand on real gear before a release:** the 90-minute iPhone (Blackmagic Camera) run over Wi-Fi with zero
interventions; the Bluetooth mic switch mid-stream; cellular from a moving car (hold, slate, webhook); two cameras
with Tentacle or Deity timecode boxes in a sync group; rapid settings changes, hide/show, duplicate scenes, closing
OBS mid-reconnect.

---

## 11. Order of work (one PR per step)

**Milestone 0: foundations**
1. Move to obs-plugintemplate (B1) with CI on all three platforms, no behaviour change.
2. Engine/glue split (E1, E3), probe CLI (E5), harness skeleton, with T1, T2, T6 and T11 passing on current behaviour.

**Milestone 1: rock solid source**
3. Timing: R1 to R3 (T3, T7, T9).
4. Audio normalization: R7 to R9 (T8).
5. Soft clock correction: R4 to R6 (T12).
6. Signal loss: R10 to R13 with the slate (T6, T10, plus a visual check in OBS).
7. Hardware decode: R19, R20.
8. Stats, Reconnect now, log prefixes: R16 to R18.

**Milestone 2: hub**
9. Server profiles and credentials: H1 to H4.
10. API client and poller: E2, H5 to H7 (T14).
11. CTD Hub dock, streams list and add to scene: H9 to H12, H14. Then, in its own PRs, CTD Multi-Stream:
    pairing, profile and status/chat panel (H18, H19, H23, H24, H26), then destinations and overlays (H20 to H22).
    H25's issue waits for step 13.
12. Many feeds, auto-add, optional config writes: H8, H13, H15 to H17, plus the obs-websocket vendor skeleton (I1)
    with the requests for this milestone.

**Milestone 3: issues**
13. Issue engine, panel and events: D1 to D3, I3.
14. Rules and fix actions: section 6.2, D4, D5 (T4, T16).

**Milestone 4: audio repair**
15. A1 to A6 (T15).

**Milestone 5: delay and sync**
16. Delay queue and manual delays: S1 to S3 (T20).
17. LTC decoder and timecode input: S4 to S8 (T17).
18. Sync groups: S9 to S15 (T18, T19).
19. Sync check: S16.

**Release**
20. Webhook (I4), API docs (I5), installers (B2), README with setup in five minutes, then T13 and the by-hand tests.

## 12. Out of scope
- Inputs other than SRT from MediaMTX (MediaMTX converts other protocols, so publishers can still use them).
- Several streams in one source (add more sources).
- Recording, replay, DVR.
- Switching scenes automatically. The events (I3) let CTD Live Studio, OBS WebSocket scripts or advanced scene
  switchers do it.
- Audio processing beyond section 7 (OBS filters do it).
- Replacing MediaMTX or running it inside OBS.
