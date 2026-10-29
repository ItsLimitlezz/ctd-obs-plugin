#!/usr/bin/env python3
"""Checks one scenario's probe output (JSON lines) against when the feed became available again.

usage: check.py PROBE_JSONL BOUND_MS [READY_EPOCH_MS ...]

For each READY time (the publisher or the server back up, as the scenario saw it), the probe must be LIVE again within
BOUND_MS: either it never left LIVE, or its next LIVE state comes no later than READY + BOUND_MS. The probe's own
bounds (its "result" line) must also have passed. Exit 0 pass, 1 fail.
"""
import json
import sys


def main() -> int:
    path, bound = sys.argv[1], float(sys.argv[2])
    ready_times = [float(x) for x in sys.argv[3:]]
    lines = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("{"):
                lines.append(json.loads(line))

    start = next((l["epochMs"] for l in lines if l.get("type") == "start"), None)
    result = next((l for l in lines if l.get("type") == "result"), None)
    if start is None or result is None:
        print("FAIL: the probe didn't start or didn't finish")
        return 1
    states = [(start + l["t"] * 1000.0, l["state"]) for l in lines if l.get("type") == "state"]

    ok = bool(result.get("pass"))
    if not ok:
        print(f"FAIL: probe bounds: {result.get('why')}")
    for ready in ready_times:
        before = [s for t, s in states if t <= ready]
        if before and before[-1] == "LIVE":
            print(f"  ready at +{(ready - start) / 1000:.2f} s: never left LIVE")
            continue
        after = [t for t, s in states if t > ready and s == "LIVE"]
        if not after:
            print(f"  ready at +{(ready - start) / 1000:.2f} s: never LIVE again")
            ok = False
            continue
        delay = after[0] - ready
        verdict = "ok" if delay <= bound else "TOO SLOW"
        print(f"  ready at +{(ready - start) / 1000:.2f} s: LIVE {delay / 1000:.2f} s later ({verdict})")
        ok = ok and delay <= bound
    print(
        f"{'PASS' if ok else 'FAIL'}  first LIVE {result.get('firstLiveS')} s, worst lateness "
        f"{result.get('worstLatenessMs')} ms, gaps {result.get('gaps')} (worst {result.get('worstGapS')} s), "
        f"reconnects {result.get('reconnects')}, resyncs {result.get('resyncs')}"
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
