# One image for the publisher (ffmpeg) and the probe (ctd-live-probe, built from this repo's engine).
# Ubuntu's FFmpeg is built with libsrt, which is all the engine needs to speak srt://.
FROM ubuntu:24.04
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ffmpeg build-essential cmake pkg-config \
      libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
 && rm -rf /var/lib/apt/lists/*
COPY src/engine /src/src/engine
COPY tools/ctd-live-probe /src/tools/ctd-live-probe
RUN cmake -S /src/tools/ctd-live-probe -B /build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build -j \
 && cmake --install /build
COPY test/publish.sh /usr/local/bin/publish.sh
RUN chmod +x /usr/local/bin/publish.sh
