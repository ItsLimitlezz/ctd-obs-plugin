# Failover on dead feeds

When a feed that is on program goes down, CTD OBS cuts to another scene by itself. **CTD ▸ Failover…** sets it up
(**Tools ▸ CTD failover** in stock OBS, or in a CTD OBS whose CTD menu doesn't have it yet). It is off until you turn it on, and its settings belong to the scene collection (they name its scenes and feeds).

## What counts as a dead feed

A feed is a **CTD Live Source** with video. Per feed, any of:

| Trigger | Means | Default |
| --- | --- | --- |
| No signal | The source has no picture: no signal, reconnecting, or never connected. A source you disconnected on purpose is not watched. | on |
| Colour bars | The encoder still sends, but it sends bars (SMPTE, EBU, or the HD pattern with grey pillars). | on |
| A saved picture | The feed shows a picture you saved with **Save what it shows now**, such as a camera's "no input" slate. | off until you save one |
| Frozen | Frames keep arriving but the picture has not changed for the time you set (10 s). | off |

The feed must stay dead for the **wait** (2 s) before anything happens, so a one-second drop is not a switch.

## Where program goes

1. The feed's own scene (**Go to**), if it has one and that scene shows no dead feed.
2. Otherwise the first **fallback scene**, from the top, that exists, is not the scene on program, and shows no dead
   feed. Put a scene with no feed at all (a logo, "be right back") last and there is always somewhere to go.
3. If nothing qualifies, program stays where it is, and the plugin looks again half a second later.

A feed counts for a scene when it is visible in it, also through a group or a nested scene. The switch is an ordinary
cut through OBS: it uses the current transition, and in Studio Mode it moves program and leaves preview alone.

## The operator always wins

- Cut back to the scene the plugin just left while its feed is still dead, and the plugin leaves that scene alone
  until the feed has recovered.
- Move anywhere else after a failover and nothing is "returned" later.
- **Go back when the feed has been healthy** (off by default) returns to the original scene once the feed that went
  down has been healthy for the time you set (5 s) and nothing else in that scene is dead.
- If you cut somewhere else in the moment between the plugin deciding and the switch landing, your cut stands: the
  switch is dropped (and logged), not made on top of yours.
- For 15 s after OBS starts or the scene collection changes, nothing is switched: feeds need a moment to connect.
- After a switch, the next one is at least 3 s away.
- While failover is off, no feed is read. Turning it on starts every feed's timer from zero, so nothing switches
  before a feed has been dead for the full wait.

## Who hears about it

Every switch is written to the OBS log, sent to obs-websocket as the vendor event `FailoverTriggered`, to the webhook,
and to CTD Relay as `obs.failover` (Relay API 1.12), which is how CTD Live tells the operator. The issues the plugin
reports carry `onProgram` and `scene`, so a dead feed that viewers can see is told apart from one they can't.

## Limits in this version

- The main canvas only. The vertical canvas does not fail over yet.
- CTD Live Sources only. Other sources (capture cards, media files, browser sources) are not watched.
- Pictures are compared on brightness in a 16 × 16 grid, which is enough to recognise bars or a slate, not to tell
  two similar camera shots apart.

## For developers

The rules are `src/obs/failover-core.c`, pure C with tests (`test/failover`, target `ctd-failover-tests`). The OBS
side is `src/obs/failover.c`; the window is `src/ui/failover-dialog.cpp`. Settings are saved in the scene
collection under `ctd-failover`:

```json
{ "enabled": true, "delayMs": 2000, "frozenMs": 10000, "tolerance": 6,
  "returnWhenBack": false, "returnAfterMs": 5000, "watchAll": true,
  "scenes": [{ "name": "Wide" }, { "name": "BRB" }],
  "sources": [{ "source": "Club cam", "watch": true, "signalLost": true, "bars": true, "frozen": false,
                "picture": "<512 hex digits, when saved>", "target": "DJ" }] }
```
