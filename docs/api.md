# CTD OBS Plugin API

The plugin can be driven and read three ways:

| Way | For | How |
|---|---|---|
| **obs-websocket vendor `ctd`** | anything that already talks to OBS (CTD Live Studio, scripts, Stream Deck, an overlay) | `CallVendorRequest` with `vendorName: "ctd"`; events arrive as `VendorEvent` |
| **Webhook** | things that don't speak OBS WebSocket (a graphics server, a bot) | set a URL on a server profile (Tools ▸ CTD Live servers); events are POSTed as JSON |
| **CTD Relay** | CTD Live mobile, CTD Overlay and the rest of the CTD family | pair in Docks ▸ CTD Relay; events go to Relay's `/apps/{appId}/events` |

Every request answers with JSON. A request that can fail has `"ok": true|false`, and an `"error"` (or `"message"`) when it
did. `serverId` is a server profile id from `GetServers`; leave it out for the default profile.

## Calling a request

obs-websocket 5, from any client:

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "1",
    "requestData": {
      "vendorName": "ctd",
      "requestType": "GetFeeds",
      "requestData": {}
    }
  }
}
```

The answer is in `d.responseData.responseData`. To receive the events, identify with the `Vendors` event subscription
(`eventSubscriptions` bit 9, value 512); they arrive as `VendorEvent` with `vendorName: "ctd"`, `eventType` and
`eventData`.

## Requests

### Servers and streams

**`GetServers`** — the server profiles. Secrets (passwords, tokens, the webhook URL) are never included.

```json
{"servers": [{"id": "srv-…", "name": "NAS (MediaMTX)", "host": "192.168.1.50", "srtPort": 8890,
  "apiUrl": "http://192.168.1.50:9997", "isDefault": true, "allowWrites": false}]}
```

**`GetStreams`** `{serverId?}` — the server's paths, from its poller (every 2 s).

```json
{"serverId": "srv-…", "status": "connected", "version": "v1.21.2", "latencyMs": 12,
 "streams": [{"name": "mob944", "live": true, "publisher": "srtConn", "remote": "10.0.0.7:51432",
   "tracks": "H264, MPEG-4 Audio", "bitrateKbps": 6120.5, "readers": 2, "rttMs": 38.2, "lossPercent": 0.1}]}
```

**`GetPublishUrl`** `{serverId?, path, protocol?}` — the URL a camera publishes `path` to. `protocol` is `srt` (default),
`rtmp` or `whip`; MediaMTX's default ports; no credentials added.

```json
{"ok": true, "url": "srt://192.168.1.50:8890?streamid=publish:mob944"}
```

**`KickPublisher`** `{serverId?, path}` — drops the camera publishing to `path` so it reconnects. Only on a profile with
"Allow CTD Hub to change this MediaMTX".

```json
{"ok": true, "message": "Kicked the publisher of \"mob944\": the camera should reconnect."}
```

### Feeds (CTD Live sources)

**`GetFeeds`** — every CTD source, each as `GetFeed`'s `feed`. **`GetFeed`** `{sourceName}`:

```json
{"feed": {
  "sourceName": "mob944", "kind": "video", "serverId": "srv-…", "host": "192.168.1.50", "port": 8890, "path": "mob944",
  "state": "LIVE",
  "fps": 29.97, "width": 1920, "height": 1080, "videoCodec": "h264", "audioCodec": "aac", "sampleRate": 48000,
  "channels": 2, "decoder": "d3d12va", "decoderSetting": 0, "nominalFps": 29.97, "decodeMs": 1.8,
  "latenessMs": 24, "correctionPpm": -120, "latencyMs": 120, "reconnects": 0, "resyncs": 0, "autoFix": false,
  "peakDbLeft": -9.5, "peakDbRight": -9.8, "clippedPercent": 0.0, "frozenMs": 0,
  "audioChannels": 0, "fillGaps": true, "lockAudio": false, "rateOverride": 0, "videoOnly": false,
  "gapsFilled": 0, "gapMsFilled": 0, "audioLockPpm": 0,
  "videoDelayMs": 0, "audioDelayMs": 0, "videoDelaySetMs": 0, "audioDelaySetMs": 0, "videoDelayLimitMs": 4800,
  "delayMemoryMb": 0.0,
  "timecodeSource": 2, "timecodeLocked": true, "timecode": "10:12:44:07", "timecodeFps": 29.97, "timecodeDrop": true,
  "timecodeUserBits": 0, "timecodeLevelDb": -6.1, "timecodeErrors": 0.0, "timecodeDetectedChannel": 0,
  "syncGroup": "stage", "syncState": "locked", "syncOffsetMs": 182.4, "syncDelayMs": 182, "syncReference": false,
  "groupDelayMs": 182,
  "syncCheckState": "idle", "syncCheckOffsetMs": 0, "syncCheckPairs": 0}}
```

`state` is `CONNECTING`, `LIVE`, `STALLED`, `RECONNECTING`, `NO SIGNAL`, `DISCONNECTED` (not in an active scene, on
purpose) or `STOPPED`.

**`ReconnectFeed`** `{sourceName}` — drops the connection and dials again at once.

**`AddFeedToScene`** `{serverId?, path, sceneName?, sourceName?}` — a CTD Live Source for `path`, added to `sceneName`
(the current program scene by default) and fitted to the canvas.

```json
{"ok": true, "sourceName": "mob944 2"}
```

### Issues

**`GetIssues`** — the raised issues, newest first.

```json
{"issues": [{"id": "frozen-picture|mob944", "rule": "frozen-picture", "severity": "warning", "source": "mob944",
  "serverId": "srv-…", "path": "mob944", "title": "Frozen picture on mob944",
  "cause": "Frames keep arriving but the picture hasn't changed for 7 s: …", "firstSeen": 1761043200000,
  "lastSeen": 1761043207000, "appliedFix": false,
  "actions": [{"id": "kick", "label": "Kick the publisher so the camera reconnects"}]}]}
```

**`ApplyFix`** `{issueId, actionId}` — one of an issue's `actions`, or `undo` on an applied fix (those show with
`appliedFix: true`). `copy-url` answers with the `url` instead of copying it.

```json
{"ok": true, "message": "Raised mob944's SRT latency to 400 ms (was 120)"}
```

### Audio, delays and timecode

**`SetAudioRepair`** `{sourceName, channels?, fillGaps?, lockAudio?, rateOverride?, videoOnly?}` — only the fields
given change. `channels`: 0 stereo, 1 left on both, 2 right on both, 3 mono, 4 swap, 5 invert right. `rateOverride`: 0,
44100 or 48000. `videoOnly` reconnects the feed.

**`ResetAudio`** `{sourceName}` — rebuilds the audio decoder, video untouched.

**`SetDelays`** `{sourceName, videoMs?, audioMs?}` — 0 to 2000 each; applied smoothly, never as a jump on air.
(`SetDelay` is the same request.)

**`SetTimecodeInput`** `{sourceName, input}` — 0 none, 1 to 8 LTC on that audio channel (1 left, 2 right), 9 SEI
timecode in the video.

**`StartSyncCheck`** `{sourceName}` — a 10 s sync check while the sync clip plays in front of the camera; the answer
shows in `GetFeed` (`syncCheckState` `running` → `done` or `failed`, `syncCheckOffsetMs`: positive when the sound comes
after the picture). **`ApplySyncCheck`** `{sourceName}` sets the delays that undo it.

### Sync groups

**`GetSyncGroups`**:

```json
{"groups": [{"name": "stage", "mode": "align", "maxDelayMs": 1000, "fixedLatencyMs": 0,
  "members": [
    {"sourceName": "cam A", "state": "locked", "lost": false, "timecode": "10:12:44:07", "offsetMs": 0, "delayMs": 0,
     "reference": true},
    {"sourceName": "cam B", "state": "locked", "lost": false, "timecode": "10:12:44:12", "offsetMs": 182.4,
     "delayMs": 182, "reference": false}]}]}
```

A member's `state`: `no-timecode`, `measuring`, `locked`, `adjusting`, `freewheel` (lost its timecode) or `at-max`
(needs more than `maxDelayMs`); the last two are `lost`.

**`SetSyncGroup`** — `{sourceName, group}` puts a source in a group (`""` for none); `{group, mode?, maxDelayMs?,
fixedLatencyMs?}` changes a group's settings (`mode`: `align` or `fixed`; `maxDelayMs` up to 5000). Both can be in one
request.

### Scenes

**`GetProgramScenes`** — each canvas's program scene. `canvas` is `landscape` for OBS's main canvas, `vertical` for Aitum
Vertical's (a canvas taller than wide), otherwise the canvas's name.

```json
{"scenes": [{"canvas": "landscape", "scene": "Camera 1", "sceneUuid": "4b0c…",
             "scenes": [{"name": "Camera 1", "uuid": "4b0c…"}, {"name": "Camera 2", "uuid": "77aa…"}]},
            {"canvas": "vertical", "scene": "Vertical Cam", "sceneUuid": "9e21…",
             "scenes": [{"name": "Vertical Cam", "uuid": "9e21…"}]}]}
```

Each canvas carries `scenes`, its whole scene list (groups left out), so an app can offer every scene before OBS ever
switches to it. Past 80 scenes the list stops there and `scenesTruncated` is `true` (CTD Relay takes at most 8 KB per
event).

## Events

| Event | When | Data |
|---|---|---|
| `FeedStateChanged` | a CTD source's state changes | `sourceName`, `path`, `serverId`, `state`, `previous` |
| `IssueRaised`, `IssueCleared` | an issue is raised, or has been gone for 10 s | `GetIssues`'s fields |
| `SyncStatusChanged` | a sync group member's state changes | `group`, `sourceName`, `state`, `previous`, `lost`, `offsetMs`, `delayMs` |
| `ProgramSceneChanged` | a canvas's program scene changes (within 100 ms; a transition counts from its start) | `canvas`, `scene`, `sceneUuid`, `previous`, `atMs`, `scenes` (the canvas's whole list) |

```json
{"eventType": "FeedStateChanged",
 "eventData": {"sourceName": "mob944", "path": "mob944", "serverId": "srv-…", "state": "STALLED", "previous": "LIVE"}}
```

## Webhook

Set **Webhook URL** on a server profile. The events above are POSTed to it as:

```json
{"source": "ctd-obs-plugin", "type": "FeedStateChanged", "timeMs": 1761043200000,
 "data": {"sourceName": "mob944", "path": "mob944", "serverId": "srv-…", "state": "STALLED", "previous": "LIVE"}}
```

An event with a `serverId` goes to that profile's webhook only; the others (scene changes, sync groups) go to every
profile's. One background thread sends them in order with a 1 s timeout each. Fire and forget: nothing is retried, a
failing webhook is logged once a minute (never its URL, which may carry a token), past 200 waiting the oldest are
dropped, and playback is never affected.

## CTD Relay events (API v1.4)

With the CTD Relay dock paired (and "Send OBS events" granted), the plugin also posts to CTD Relay, which passes them
on to its other apps:

| Event | When | Data |
|---|---|---|
| `obs.feed.state` | a CTD source's state changes | `source` {`name`, `path`, `server`}, `state`, `previous`, `gapMs` (back live after a gap) |
| `obs.feed.stats` | every 5 s while LIVE | `source`, `fps`, `width`, `height`, `latenessMs`, `resyncs`, `reconnects`, `decoder` |
| `obs.output` | the main stream, the recording or another streaming output (Aitum Vertical's) changes | `output`, `canvas`, `state` (`live`, `reconnecting`, `stopped`), `error` |
| `obs.scene` | a canvas's program scene changes; its scene list changes; everything again every minute | `canvas`, `scene`, `sceneUuid`, `previous` (on a change of scene only), `atMs`, `scenes` [{`name`, `uuid`}] |
| `obs.issue.raised`, `obs.issue.cleared` | as `IssueRaised`, `IssueCleared` | the same |
| `obs.sync.status` | as `SyncStatusChanged` | the same |

Events wait in a small queue while CTD Relay is away and never affect playback.
