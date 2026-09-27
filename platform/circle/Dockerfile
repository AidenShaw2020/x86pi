FROM debian:12
RUN apt-get update && apt-get install -y --no-install-recommends \
    make git gcc g++ gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work/platform/circle
