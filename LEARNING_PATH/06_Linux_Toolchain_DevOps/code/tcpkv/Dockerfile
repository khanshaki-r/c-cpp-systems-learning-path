# syntax=docker/dockerfile:1.7
# tcpkv multi-stage build.
#   docker build -t tcpkv:dev .                   -> runtime image (~90 MB)
#   docker build --target build -t tcpkv:build .  -> build image with compiler, for CI and debugging
#   docker run --rm -p 7000:7000 tcpkv:dev        -> serve on 7000
#   docker run --rm -p 7001:7001 tcpkv:dev 7001   -> CMD override: serve on 7001

# ============================================================ Stage 1: build
# Pin a digest in production:  FROM ubuntu:24.04@sha256:<digest> AS build
FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive

# One RUN: update + install + clean, so the package index never ends up in a committed layer.
# --no-install-recommends keeps the toolchain layer small. ca-certificates is needed for the
# FetchContent https clone of GoogleTest; git for the clone itself.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy build-system files first: the configure step (which downloads GoogleTest into
# build/_deps) is cached as long as these do not change.
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY tests/CMakeLists.txt tests/CMakeLists.txt

# Sources last: editing a .cpp invalidates only the layers from here down.
COPY include/ include/
COPY src/ src/
COPY tests/ tests/
COPY deploy/ deploy/

# Configure, build, TEST, install. A failing test fails the image build: we never ship a binary
# that did not pass its tests in exactly this environment.
# --strip installs a stripped binary. The unstripped one stays in build/release for symbolising
# cores; CI uploads it as an artifact (module 09).
RUN cmake --preset release \
    && cmake --build --preset release \
    && ctest --preset release \
    && cmake --install build/release --prefix /opt/tcpkv --strip \
    && ls -l /opt/tcpkv/bin

# ============================================================ Stage 2: runtime
FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

# Runtime deps only. `ldd /opt/tcpkv/bin/tcpkv` in the build stage lists what is needed:
#   libstdc++.so.6 -> libstdc++6, libgcc_s.so.1 -> libgcc-s1 (already in base), libc -> base.
# tini: a tiny PID-1 init that forwards signals and reaps zombies.
# netcat-openbsd: only for the HEALTHCHECK below; drop it if you add a native health command.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        ca-certificates \
        tini \
        netcat-openbsd \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system tcpkv \
    && useradd --system --gid tcpkv --no-create-home --shell /usr/sbin/nologin tcpkv

# The only thing taken from the build stage.
COPY --from=build /opt/tcpkv/bin/tcpkv /usr/local/bin/tcpkv

LABEL org.opencontainers.image.title="tcpkv" \
      org.opencontainers.image.description="TCP key-value server (learning project)" \
      org.opencontainers.image.source="https://github.com/<user>/tcpkv" \
      org.opencontainers.image.licenses="MIT"

# Never run the server as root inside the container.
USER tcpkv

# Documentation for humans and `docker run -P`; does not publish anything by itself.
EXPOSE 7000

# A real protocol check, not just "is the port open": a deadlocked server fails this.
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD sh -c 'printf "PING\n" | nc -w 1 127.0.0.1 7000 | grep -q PONG'

# ENTRYPOINT is the program; CMD holds overridable default arguments.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/tcpkv"]
CMD ["7000"]
