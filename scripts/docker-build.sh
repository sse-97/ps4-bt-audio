#!/bin/sh
# Build inside the pinned Docker image, the same way CI does.
#   scripts/docker-build.sh            # make dist: payload, PKG and SHA256SUMS in build/dist
#   scripts/docker-build.sh payload    # any Makefile target
set -eu
cd "$(dirname "$0")/.."
docker build -t ps4-bt-audio-build docker
exec docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:/src" -w /src \
    ps4-bt-audio-build make "${@:-dist}"
