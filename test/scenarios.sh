#!/usr/bin/env bash
# The harness scenarios (SPEC 10.4) on Docker Compose. Needs docker, curl and python3.
#
#   test/scenarios.sh            T1 T2 T6 T11 T15 T17 T18 T19 T20
#   test/scenarios.sh T1 T11     just those
#   T6_MINUTES=3 test/scenarios.sh T6   (the spec's 15 minutes is the default)
#
# Each scenario starts from a fresh server, runs ctd-live-probe with --json into test/out/<T>.jsonl, and checks it with
# check.py. The exit code is the number of scenarios that failed.
set -uo pipefail
cd "$(dirname "$0")"
mkdir -p out

API="http://localhost:9997/v3/paths/get/cam1"
RECOVER_MS=3000 # "LIVE within 3 s" of the feed being back

dc() { docker compose "$@"; }
now_ms() { date +%s%3N; }

# the server has the publisher's stream, as a reader would see it
wait_ready() {
  for _ in $(seq 1 300); do
    if curl -sf "$API" | grep -q '"ready":true'; then
      now_ms
      return 0
    fi
    sleep 0.1
  done
  echo "timeout"
  return 1
}

# a given path is ready (the sync group scenarios publish to cam1, cam2 and cam3)
wait_path() {
  for _ in $(seq 1 300); do
    curl -sf "http://localhost:9997/v3/paths/get/$1" | grep -q '"ready":true' && return 0
    sleep 0.1
  done
  return 1
}

# an extra LTC publisher on a path, by name: publish_ltc NAME PATH "ltc-gen options"
publish_ltc() {
  dc run -d --rm --name "ctd-$1" -e PUBLISH_URL="srt://mediamtx:8890?streamid=publish:$2&pkt_size=1316" \
    -e PUBLISH_LTC="$3" -e PUBLISH_SIZE=640x360 publisher >/dev/null
}

stop_ltc() { docker container stop -t 0 "$@" >/dev/null 2>&1; }

wait_api() {
  for _ in $(seq 1 300); do
    curl -s -o /dev/null "http://localhost:9997/v3/paths/list" && return 0
    sleep 0.1
  done
  return 1
}

fresh() {
  dc down -t 1 >/dev/null 2>&1 || true
  dc up -d mediamtx >/dev/null
  wait_api
}

probe() { dc run --rm -T probe ctd-live-probe --host mediamtx --path cam1 --json "$@"; }

# T1: cold start with the publisher already live: LIVE within 3 s; B-frames on, so video timestamps must also stay
# strictly increasing (T3's check comes free).
T1() {
  fresh
  dc up -d publisher >/dev/null
  wait_ready >/dev/null || return 1
  probe --seconds 10 --live-within 3 --monotonic --max-lateness 700 >out/T1.jsonl
  python3 check.py out/T1.jsonl "$RECOVER_MS"
}

# T2: the probe starts first; LIVE within 3 s of the publisher starting.
T2() {
  fresh
  probe --seconds 20 --monotonic --max-lateness 700 >out/T2.jsonl &
  local pid=$!
  sleep 6
  dc up -d publisher >/dev/null
  local ready
  ready=$(wait_ready) || return 1
  wait "$pid"
  python3 check.py out/T2.jsonl "$RECOVER_MS" "$ready"
}

# T6: a 3 s outage every 60 s: each time LIVE again within 3 s of the publisher returning, and no accumulation
# (lateness never past the drift cap).
T6() {
  local minutes="${T6_MINUTES:-15}"
  fresh
  dc up -d publisher >/dev/null
  wait_ready >/dev/null || return 1
  probe --seconds $((minutes * 60 + 15)) --max-lateness 700 >out/T6.jsonl &
  local pid=$!
  local readies=()
  for _ in $(seq 1 "$minutes"); do
    sleep 57
    dc stop -t 0 publisher >/dev/null
    sleep 3
    dc start publisher >/dev/null
    readies+=("$(wait_ready)")
  done
  wait "$pid"
  python3 check.py out/T6.jsonl "$RECOVER_MS" "${readies[@]}"
}

# T11: MediaMTX restarted under a live feed: back within the reconnect delay (1 s) plus 3 s of the stream being
# available again, and the API answering again.
T11() {
  fresh
  dc up -d publisher >/dev/null
  wait_ready >/dev/null || return 1
  probe --seconds 40 --max-lateness 700 >out/T11.jsonl &
  local pid=$!
  sleep 10
  dc restart -t 0 mediamtx >/dev/null
  wait_api || { echo "FAIL: the API didn't come back"; return 1; }
  local ready
  ready=$(wait_ready) || return 1
  wait "$pid"
  python3 check.py out/T11.jsonl $((RECOVER_MS + 1000)) "$ready"
}

# T15 (section 7, A2): every 20th AAC packet dropped on the way in. The holes are filled with silence (with fades, so
# no clicks) and the feed stays LIVE with its lateness in bounds.
T15() {
  fresh
  PUBLISH_AUDIO_BSF="noise=drop=not(mod(n\,20))" dc up -d publisher >/dev/null
  wait_ready >/dev/null || return 1
  probe --seconds 15 --live-within 3 --max-lateness 700 --min-gaps-filled 10 >out/T15.jsonl
  local status=$?
  python3 check.py out/T15.jsonl "$RECOVER_MS" && [ "$status" -eq 0 ]
}

# T20 (S1, S2): the delays go 0 → 1500 ms video and 0 → 800 ms audio while live. Video timestamps never go back, the
# feed never leaves LIVE, both delays get there, the audio is stretched rather than left with holes, and the queue stays
# inside its memory cap.
T20() {
  fresh
  dc up -d publisher >/dev/null
  wait_ready >/dev/null || return 1
  probe --seconds 45 --live-within 3 --monotonic --max-lateness 700 --change-delay-at 10 \
    --video-delay-to 1500 --audio-delay-to 800 --reach-delays --no-gaps-filled >out/T20.jsonl
  local status=$?
  python3 check.py out/T20.jsonl "$RECOVER_MS" && [ "$status" -eq 0 ]
}

# T17 (S5, S8): LTC on the right channel through AAC at 64, 128 and 256 kbps, at every supported rate. The timecode
# locks at the right rate (drop frame when it is) with under 0.1% of the words lost. The probe starts 3 s after the
# stream does: a freshly started AAC encoder mangles a word or two of LTC whenever the other channel's beep starts in
# its first two seconds (a camera's encoder has long been running).
T17() {
  local failed=0 kbps rate
  for kbps in 64 128 256; do
    for rate in "23.976" "24" "25" "29.97 --drop" "29.97" "30"; do
      fresh
      PUBLISH_LTC="--fps $rate" PUBLISH_AUDIO_KBPS="$kbps" dc up -d publisher >/dev/null
      wait_ready >/dev/null || return 1
      sleep 3
      local fps="${rate%% *}" drop=""
      [ "$rate" != "$fps" ] && drop="--expect-drop"
      echo "-- $kbps kbps, $rate"
      probe --seconds 15 --live-within 3 --timecode 2 --expect-tc-fps "$fps" $drop --max-tc-errors 0.1 \
        >"out/T17-$kbps-${rate// /}.jsonl"
      local status=$?
      python3 check.py "out/T17-$kbps-${rate// /}.jsonl" "$RECOVER_MS" && [ "$status" -eq 0 ] || failed=1
    done
  done
  # S7: with no timecode input set, the LTC is found on its channel
  fresh
  PUBLISH_LTC="--fps 25" dc up -d publisher >/dev/null
  wait_ready >/dev/null || return 1
  echo "-- LTC found with the input set to None"
  probe --seconds 6 --expect-detected 2 >out/T17-detect.jsonl || failed=1
  return "$failed"
}

# T18 (S9 to S12): three feeds with time-of-day LTC arriving 0, 180 and 420 ms late lock within 10 s, then stay within
# a frame of each other on screen, with no jump after the first lock.
T18() {
  fresh
  publish_ltc a cam1 "--fps 25 --time-of-day"
  publish_ltc b cam2 "--fps 25 --time-of-day --offset-ms 180"
  publish_ltc c cam3 "--fps 25 --time-of-day --offset-ms 420"
  wait_path cam1 && wait_path cam2 && wait_path cam3 || return 1
  sleep 3
  dc run --rm -T probe ctd-live-probe --host mediamtx --group cam1,cam2,cam3 --json --seconds 30 --lock-within 10 \
    --max-spread-frames 1 --no-jumps-after-lock --all-locked-at-end >out/T18.jsonl
  local status=$?
  stop_ltc ctd-a ctd-b ctd-c
  tail -n 1 out/T18.jsonl
  return "$status"
}

# T19 (S13): the 180 ms member loses its LTC for 7 s, then its link for 3 s; both come back. It freewheels, relocks
# without a jump, and the group ends locked within a frame. (A restarted publisher comes back later than before: its
# ffmpeg starting up. Hence a 3 s max delay and time to ramp there.)
T19() {
  fresh
  publish_ltc a cam1 "--fps 25 --time-of-day"
  publish_ltc b cam2 "--fps 25 --time-of-day --offset-ms 180 --mute-from 18 --mute-to 25"
  publish_ltc c cam3 "--fps 25 --time-of-day --offset-ms 420"
  wait_path cam1 && wait_path cam2 && wait_path cam3 || return 1
  sleep 3
  dc run --rm -T probe ctd-live-probe --host mediamtx --group cam1,cam2,cam3 --json --seconds 75 --max-delay 3000 \
    --max-spread-frames 1 --no-jumps-after-lock --expect-freewheel --all-locked-at-end >out/T19.jsonl &
  local pid=$!
  sleep 27
  stop_ltc ctd-b
  sleep 3
  publish_ltc b cam2 "--fps 25 --time-of-day --offset-ms 180"
  wait "$pid"
  local status=$?
  stop_ltc ctd-a ctd-b ctd-c
  tail -n 1 out/T19.jsonl
  return "$status"
}

# T21 (S16): the sync check measures the publisher's A/V offset: 0, and 200 ms with the beep held back that much
# (within 40 ms).
T21() {
  local failed=0 offset expect
  for offset in 0 0.2; do
    expect=$(awk "BEGIN { print $offset * 1000 }")
    fresh
    PUBLISH_AUDIO_OFFSET="$offset" dc up -d publisher >/dev/null
    wait_ready >/dev/null || return 1
    probe --seconds 16 --sync-check-at 3 --expect-av-offset "$expect" >"out/T21-$expect.jsonl" || failed=1
    tail -n 1 "out/T21-$expect.jsonl"
  done
  return "$failed"
}

scenarios=("$@")
[ ${#scenarios[@]} -eq 0 ] && scenarios=(T1 T2 T6 T11 T15 T17 T18 T19 T20 T21)
dc build >/dev/null || exit 100
failed=0
for t in "${scenarios[@]}"; do
  echo "== $t"
  if ! "$t"; then
    failed=$((failed + 1))
  fi
done
dc down -t 1 >/dev/null 2>&1 || true
echo "== ${failed} of ${#scenarios[@]} scenarios failed"
exit "$failed"
