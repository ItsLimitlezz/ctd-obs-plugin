#!/bin/sh
# The harness publisher (SPEC 10.4): H.264 with B-frames and AAC over SRT, carrying the test signal, a white flash and
# a 1 kHz beep at the top of every second, on the same timestamp. Runs until killed or the server drops it.
#   PUBLISH_URL   srt://mediamtx:8890?streamid=publish:cam1&pkt_size=1316
#   PUBLISH_SIZE  1280x720 (the spec's 1080p needs a runner fast enough to encode it in real time)
#   PUBLISH_FPS   30
#   PUBLISH_AUDIO_BSF  an audio bitstream filter, to damage the audio on purpose (T15 drops every 20th AAC packet)
#   PUBLISH_LTC   ltc-gen's options (e.g. "--fps 29.97 --drop"): LTC on the right channel, the beep on the left (T17)
#   PUBLISH_AUDIO_KBPS  the AAC bitrate (default 128)
#   PUBLISH_AUDIO_OFFSET  seconds the beep comes after the flash (T21 uses 0.2)
set -eu
size="${PUBLISH_SIZE:-1280x720}"
fps="${PUBLISH_FPS:-30}"
url="${PUBLISH_URL:-srt://mediamtx:8890?streamid=publish:cam1&pkt_size=1316}"
kbps="${PUBLISH_AUDIO_KBPS:-128}"
video="testsrc2=size=${size}:rate=${fps},drawbox=c=white:t=fill:enable='lt(mod(t\,1)\,1/${fps})'"
if [ -n "${PUBLISH_LTC:-}" ]; then
  # shellcheck disable=SC2086 # the options are meant to split
  ltc-gen $PUBLISH_LTC | exec ffmpeg -hide_banner -loglevel warning \
    -re -f lavfi -i "$video" \
    -re -f s16le -ar 48000 -ac 2 -i pipe:0 \
    -map 0:v -map 1:a \
    -c:v libx264 -preset veryfast -bf 2 -g "$((fps * 2))" -b:v 3M -pix_fmt yuv420p \
    -c:a aac -b:a "${kbps}k" \
    -f mpegts "$url"
  exit
fi
exec ffmpeg -hide_banner -loglevel warning -re \
  -f lavfi -i "$video" \
  -itsoffset "${PUBLISH_AUDIO_OFFSET:-0}" -f lavfi -i "aevalsrc='if(lt(mod(t\,1)\,0.05)\,sin(2*PI*1000*t)\,0)':s=48000:c=stereo" \
  -c:v libx264 -preset veryfast -bf 2 -g "$((fps * 2))" -b:v 3M -pix_fmt yuv420p \
  -c:a aac -b:a "${kbps}k" ${PUBLISH_AUDIO_BSF:+-bsf:a "$PUBLISH_AUDIO_BSF"} \
  -f mpegts "$url"
