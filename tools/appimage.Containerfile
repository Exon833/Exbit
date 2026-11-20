# The system the AppImage's program is built on (tools/appimage.sh): Ubuntu 22.04, so it needs only glibc 2.35
# or newer (most distributions from 2022 on); g++-12 for the C++ (its libstdc++ is linked in), the headers of
# X11, ALSA and PulseAudio (their libraries are loaded at run time, never linked), git for the commit id.
FROM docker.io/library/ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++-12 make git ca-certificates \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libasound2-dev libpulse-dev \
    && rm -rf /var/lib/apt/lists/*
