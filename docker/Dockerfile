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
 && rm -rf /var/lib/apt/lists/* \
 && ln -s /usr/bin/clang-18 /usr/local/bin/clang \
 && ln -s /usr/bin/ld.lld-18 /usr/local/bin/ld.lld

# PkgTool.Core ships its own (old) .NET runtime, which needs OpenSSL 1.1.
# Ubuntu 24.04 only has OpenSSL 3, so take the 20.04 release package
# (checksum from the focal Packages index).
ARG LIBSSL_DEB=libssl1.1_1.1.1f-1ubuntu2_amd64.deb
ARG LIBSSL_SHA256=09ee28588a1fb5613ddc6c26a992d5a76931b3cf22c022930da413a5e580599e
RUN curl -fsSL "http://archive.ubuntu.com/ubuntu/pool/main/o/openssl/$LIBSSL_DEB" -o /tmp/libssl.deb \
 && echo "$LIBSSL_SHA256  /tmp/libssl.deb" | sha256sum -c - \
 && dpkg -i /tmp/libssl.deb && rm /tmp/libssl.deb

# make_assets.py bakes the UI fonts from the DejaVu copy bundled with matplotlib.
COPY requirements.txt /tmp/requirements.txt
RUN pip3 install --no-cache-dir --break-system-packages -r /tmp/requirements.txt

# OpenOrbis PS4 toolchain (headers, libs, create-fself, create-gp4, PkgTool).
ARG OO_VERSION=v0.5.4
ARG OO_URL=https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/${OO_VERSION}/toolchain-llvm-18.tar.gz
ARG OO_SHA256=3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526
RUN curl -fsSL "$OO_URL" -o /tmp/oo.tar.gz \
 && echo "$OO_SHA256  /tmp/oo.tar.gz" | sha256sum -c - \
 && mkdir -p /opt/oo && tar -xzf /tmp/oo.tar.gz -C /opt/oo && rm /tmp/oo.tar.gz \
 && root="$(dirname "$(find /opt/oo -name link.x -print -quit)")" \
 && test -f "$root/link.x" && ln -s "$root" /opt/OpenOrbis \
 && chmod +x /opt/OpenOrbis/bin/linux/* \
 && ls /opt/OpenOrbis/bin/linux

# PkgTool's runtime does not know Ubuntu 24.04's ICU; it needs no locale data.
ENV OO_PS4_TOOLCHAIN=/opt/OpenOrbis \
    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

WORKDIR /src
