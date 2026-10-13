# Dockerfile -- the tracker, node and client in one image (decision D-064).
#
# Tested by scripts/e2e-compose.sh, locally and in CI on x86-64 and arm64.
#
# Built from source in the first stage, so the image is for whatever architecture
# builds it (arm64 or amd64). The runtime stage carries only the three programs
# and the OpenSSL library they link.

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make libssl-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make -j"$(nproc)" tracker node client

FROM ubuntu:24.04
# libssl3t64 is Ubuntu 24.04's name for the OpenSSL 3 runtime library (libcrypto).
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3t64 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/tracker /src/node /src/client /usr/local/bin/
# Stores and the tracker's log live in /data, which compose.yaml gives each
# container as its own volume.
VOLUME /data
