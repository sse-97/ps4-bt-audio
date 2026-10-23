# Reproducible build environment for the payload and the PKG.
# Used by scripts/docker-build.sh and by CI (.github/workflows/build.yml).
#
# Ubuntu 24.04 ships clang/lld 18.1.3, which rebuilds the released 2.7.5
# payload byte for byte. Change the base image only together with a check
# that `make payload` still matches the last release's SHA256.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl make git \
      clang-18 lld-18 \
      python3 python3-pip \
      dotnet-runtime-8.0 \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s /usr/bin/clang-18 /usr/local/bin/clang \
 && ln -s /usr/bin/ld.lld-18 /usr/local/bin/ld.lld

# make_assets.py bakes the UI fonts from the DejaVu copy bundled with matplotlib.
COPY requirements.txt /tmp/requirements.txt
RUN pip3 install --no-cache-dir --break-system-packages -r /tmp/requirements.txt

# OpenOrbis PS4 toolchain (headers, libs, create-fself, create-gp4, PkgTool).
ARG OO_VERSION=v0.5.4
ARG OO_URL=https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/${OO_VERSION}/toolchain-llvm-18.tar.gz
ARG OO_SHA256=
RUN curl -fsSL "$OO_URL" -o /tmp/oo.tar.gz \
 && if [ -n "$OO_SHA256" ]; then echo "$OO_SHA256  /tmp/oo.tar.gz" | sha256sum -c -; fi \
 && sha256sum /tmp/oo.tar.gz | tee /opt/openorbis.sha256 \
 && mkdir -p /opt/oo && tar -xzf /tmp/oo.tar.gz -C /opt/oo && rm /tmp/oo.tar.gz \
 && root="$(dirname "$(find /opt/oo -name link.x -print -quit)")" \
 && test -f "$root/link.x" && ln -s "$root" /opt/OpenOrbis \
 && chmod +x /opt/OpenOrbis/bin/linux/* \
 && ls /opt/OpenOrbis/bin/linux \
 && find /opt/oo \( -name right.sprx -o -name libc.prx -o -name libSceFios2.prx -o -name 'create-*' -o -name 'PkgTool*' \) -print

ENV OO_PS4_TOOLCHAIN=/opt/OpenOrbis \
    DOTNET_ROLL_FORWARD=Major \
    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
    DOTNET_CLI_TELEMETRY_OPTOUT=1

WORKDIR /src
