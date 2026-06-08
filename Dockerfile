# Reproducible build + live capture image for binance-lob-capture.
# Multi-stage: build with full toolchain, ship a slim runtime.

# ---- build stage ----------------------------------------------------------
FROM ubuntu:22.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build g++-12 \
      libssl-dev libboost-all-dev zlib1g-dev libsimdjson-dev \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests

# Live capture ON (Boost.Beast + OpenSSL). Release, warnings-as-info.
RUN cmake -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=g++-12 \
      -DENABLE_LIVE=ON \
      -DENABLE_SIMDJSON=ON \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure

# ---- runtime stage --------------------------------------------------------
FROM ubuntu:22.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 libboost-system1.74.0 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# simdjson shared lib (copied from build stage so we don't depend on the
# runtime package name across distro versions)
COPY --from=build /usr/lib/x86_64-linux-gnu/libsimdjson.so* /usr/lib/x86_64-linux-gnu/
RUN ldconfig
COPY --from=build /src/build/binance_capture /usr/local/bin/binance_capture
WORKDIR /work
# Outputs land in /work/output -> bind-mount a host folder there.
ENTRYPOINT ["binance_capture"]
CMD ["--help"]
