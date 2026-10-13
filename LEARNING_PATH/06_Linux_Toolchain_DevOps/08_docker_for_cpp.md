# 08. Docker for C++

**Time:** ~3 hours   **Prerequisites:** 02, 04; tcpkv builds locally

## 1. In plain words

A container is a normal Linux process that the kernel has been told to lie to: it sees its own
filesystem (from an **image**), its own process list, its own network interface and hostname, and
it can be limited in CPU and memory. It is not a virtual machine; there is no second kernel. It
starts in milliseconds because starting it is just starting a process with some namespaces set up.

An image is a read-only filesystem snapshot built in layers from a recipe called a `Dockerfile`.
"Run the image" means: stack the layers, add a writable layer on top, start the process inside.
The win for C++ is reproducibility: the exact compiler, exact library versions and exact runtime
environment are pinned in text, built once, and run identically on your WSL, your colleague's Mac,
the CI runner and the production server. "Works on my machine" becomes "works in this image".

Analogy: an image is a sealed shipping container with the application and everything it needs
packed inside; the host is the ship. The ship does not care what is inside; the container does
not care which ship it is on.

## 2. The real thing

### 2.1 Install on Windows with the WSL2 backend

Install **Docker Desktop for Windows**, enable "Use the WSL 2 based engine", and in Settings ->
Resources -> WSL Integration enable `Ubuntu-24.04`. Then inside your Ubuntu terminal:

```bash
docker version            # Client and Server sections; Server runs in Docker Desktop's own WSL distro
docker run --rm hello-world
docker run --rm -it ubuntu:24.04 bash   # a throwaway Ubuntu shell; exit to destroy
```

Alternative without Docker Desktop: install Docker Engine directly in WSL Ubuntu
(`apt install docker.io`, `sudo usermod -aG docker $USER`, enable systemd in `/etc/wsl.conf`).
Either way the commands below are identical.

### 2.2 Images, containers, registries

```
Dockerfile --docker build--> image (layers, immutable, tagged tcpkv:0.1.0)
image --docker run--> container (running process + writable layer)
image --docker push--> registry (Docker Hub, ghcr.io, Azure Container Registry)
```

```bash
docker build -t tcpkv:dev .                 # build from ./Dockerfile; -t tags it
docker images                               # local images with sizes
docker run --rm -p 7000:7000 tcpkv:dev      # run; --rm deletes the container on exit; -p host:container port
docker run -d --name kv -p 7000:7000 tcpkv:dev   # detached, named
docker ps; docker ps -a                     # running; all including exited
docker logs -f kv                           # stdout/stderr of the main process
docker exec -it kv sh                       # shell inside a running container
docker stop kv                              # SIGTERM, 10 s grace, then SIGKILL
docker rm kv; docker rmi tcpkv:dev
docker inspect kv | less                    # everything: IP, mounts, env, state
docker stats                                # live CPU/mem per container
docker system df; docker system prune       # disk usage; delete stopped containers, dangling images, build cache
docker tag tcpkv:dev ghcr.io/<user>/tcpkv:0.1.0 && docker push ghcr.io/<user>/tcpkv:0.1.0
```

A container's PID 1 is your process. When it exits, the container exits. `docker stop` sends
SIGTERM to PID 1: tcpkv's handler (module 11) drains and exits 0; a program that ignores SIGTERM
gets SIGKILL after 10 seconds and loses in-flight work. Exit code 137 in `docker ps -a` is 128+9:
SIGKILL, often the memory limit (OOM) or the stop timeout.

### 2.3 Dockerfile for a C++ build: multi-stage

A naive Dockerfile installs gcc and cmake, copies the source, builds, and runs. The image is
1.5 GB and contains the compiler, your source, and your build tree. Multi-stage builds fix this:
one stage builds, a second minimal stage copies only the installed binary.

```dockerfile
# syntax=docker/dockerfile:1.7
# ---------- Stage 1: build ----------
FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# Copy only what the configure step needs first so the dependency download layer is cached
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY tests/CMakeLists.txt tests/
# Then the rest (changes here do not invalidate the layers above)
COPY include/ include/
COPY src/ src/
COPY tests/ tests/
COPY deploy/ deploy/
RUN cmake --preset release -DTCPKV_BUILD_TESTS=ON \
    && cmake --build --preset release \
    && ctest --preset release \
    && cmake --install build/release --prefix /opt/tcpkv --strip

# ---------- Stage 2: runtime ----------
FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system tcpkv && useradd --system --gid tcpkv --no-create-home tcpkv
COPY --from=build /opt/tcpkv/bin/tcpkv /usr/local/bin/tcpkv
USER tcpkv
EXPOSE 7000
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/tcpkv"]
CMD ["7000"]
```

Line by line:

- `FROM ubuntu:24.04 AS build`: base image and stage name. Pin a digest
  (`ubuntu:24.04@sha256:...`) for byte-exact reproducibility in production.
- `apt-get` not `apt` (stable CLI), `--no-install-recommends` (smaller),
  `rm -rf /var/lib/apt/lists/*` **in the same RUN** (the layer is what is committed; deleting in a
  later layer does not shrink the image).
- `WORKDIR` creates and enters the directory.
- `COPY` order is about **cache**: Docker reuses a layer if the instruction and the copied files
  are unchanged. Put slow, rarely changing steps first. Here configure (which fetches GoogleTest)
  depends only on CMake files, so editing `src/store.cpp` does not re-download anything.
- The build RUN runs tests inside the build: an image is never produced from code that fails tests.
- `cmake --install ... --strip` installs the stripped binary to `/opt/tcpkv`. Keep the unstripped
  one as a CI artifact (module 09) for symbolising cores.
- Stage 2 starts from a clean `ubuntu:24.04` with only the runtime library `libstdc++6`
  (`ldd tcpkv` tells you what else you need: for libpq, `libpq5`; module 02).
- `tini` is a tiny init that forwards signals and reaps zombies, because PID 1 in a container has
  special signal semantics (section 4).
- `useradd --system` + `USER tcpkv`: the server does not run as root. A container escape from a
  root process is root on the host's kernel namespace; from an unprivileged user it is far less.
- `COPY --from=build`: pulls one file out of the build stage. Nothing else from stage 1 ships.
- `ENTRYPOINT` is the executable; `CMD` is the default arguments, overridable:
  `docker run tcpkv:dev 7001`.
- `EXPOSE` is documentation (and used by `-P`); it does not publish the port. `-p` does.

Result: build stage ~1.2 GB (cached locally, never shipped), runtime image ~80-100 MB (typical
for Ubuntu base + libstdc++). Going smaller: `debian:12-slim` base (~75 MB total), `alpine` with a
musl static build (~10 MB), or `gcr.io/distroless/cc-debian12` (~25 MB, no shell, has libstdc++)
or `FROM scratch` with a fully static binary (~2 MB). Trade-off: no shell means `docker exec sh`
does not work; debug with an ephemeral debug container instead (section 2.8).

```bash
docker build -t tcpkv:0.1.0 .
docker build --target build -t tcpkv:build .      # stop after stage 1 (for running tests/gdb in it)
docker build --no-cache ...                        # ignore cache
docker build --progress=plain ...                  # full output of each RUN
docker history tcpkv:0.1.0                         # layer sizes: where did the MB go
docker run --rm -p 7000:7000 tcpkv:0.1.0 && nc localhost 7000
```

### 2.4 Layers and the cache

Each instruction creates a layer: a tarball of filesystem changes plus metadata. Layers are
content-addressed and shared between images (ten images from `ubuntu:24.04` share the base). An
image is a manifest listing layers. The cache rule: a layer is reused if its parent layer is the
same and the instruction is the same; for `COPY`/`ADD`, also if the checksum of the copied files is
the same. The first changed instruction invalidates everything below it.

Consequences:

- `COPY . .` as the first instruction destroys caching: any edit anywhere rebuilds everything.
- A `.dockerignore` excludes `build/`, `.git/`, editor files so they neither invalidate the cache
  nor bloat the context sent to the daemon.
- `RUN apt-get update` alone in a layer is a trap: it caches an old package index forever; always
  combine with the `install` in one `RUN`.
- BuildKit cache mounts keep a package cache across builds without putting it in a layer:
  `RUN --mount=type=cache,target=/var/cache/apt apt-get ...`, and for ccache
  `RUN --mount=type=cache,target=/root/.ccache cmake --build ...`.

### 2.5 Volumes and bind mounts

Container filesystems are ephemeral: `docker rm` deletes the writable layer. Persistent data lives
in volumes.

```bash
docker volume create pgdata
docker run -d -v pgdata:/var/lib/postgresql/data postgres:16      # named volume (managed by Docker)
docker run --rm -v "$PWD":/src -w /src tcpkv:build cmake --preset debug   # bind mount: host dir into container
docker run --rm -v "$PWD":/src:ro ...                                      # read-only
docker volume ls; docker volume inspect pgdata; docker volume rm pgdata
```

Bind mounts from the WSL Linux filesystem are fast; from `/mnt/c` they are slow (module 01). The
"build inside a container, edit on the host" workflow is a bind mount of your source into the
build-stage image: same compiler as CI, your editor on the host.

### 2.6 Networking basics

Each container gets its own network namespace with a virtual interface. By default all containers
join the `bridge` network and can reach the internet via NAT. Ports are not reachable from the
host unless published with `-p`.

```bash
docker network create kvnet
docker run -d --name redis --network kvnet redis:7
docker run -d --name kv --network kvnet -p 7000:7000 tcpkv:0.1.0
docker exec -it kv sh -c 'getent hosts redis'      # Docker's DNS resolves container names on user networks
docker network inspect kvnet
```

`-p 7000:7000` is host:container; `-p 127.0.0.1:7000:7000` binds only to loopback on the host.
On a user-defined network, containers reach each other by **name** (`redis:6379`); on the default
bridge they cannot (legacy `--link`). `--network host` removes the namespace (Linux only; in
Docker Desktop it means the Docker VM's host). Inside a container, `localhost` is the container
itself, never the host; use `host.docker.internal` from Docker Desktop to reach the Windows host.

### 2.7 docker compose: tcpkv + PostgreSQL + Redis

Compose describes several containers, their networks and volumes in one YAML file and starts
them together. This is how you run a realistic local environment: your server plus the database
and cache it will talk to in 07_Databases_Internals and 08_Networking.

```yaml
# docker-compose.yml
services:
  tcpkv:
    build:
      context: .
      target: runtime
    image: tcpkv:dev
    ports: ["7000:7000"]
    environment:
      TCPKV_PG_DSN: "postgresql://tcpkv:tcpkv@postgres:5432/tcpkv"
      TCPKV_REDIS_URL: "redis://redis:6379/0"
    depends_on:
      postgres: { condition: service_healthy }
      redis:    { condition: service_healthy }
    restart: unless-stopped
    deploy:
      resources:
        limits: { memory: 256M, cpus: "1.0" }

  postgres:
    image: postgres:16
    environment:
      POSTGRES_USER: tcpkv
      POSTGRES_PASSWORD: tcpkv
      POSTGRES_DB: tcpkv
    volumes:
      - pgdata:/var/lib/postgresql/data
      - ./deploy/initdb:/docker-entrypoint-initdb.d:ro
    ports: ["127.0.0.1:5432:5432"]
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U tcpkv -d tcpkv"]
      interval: 5s
      timeout: 3s
      retries: 10

  redis:
    image: redis:7-alpine
    command: ["redis-server", "--appendonly", "yes"]
    volumes: ["redisdata:/data"]
    ports: ["127.0.0.1:6379:6379"]
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 5s
      timeout: 3s
      retries: 10

volumes:
  pgdata:
  redisdata:
```

```bash
docker compose up -d --build        # build tcpkv, start all three, in dependency order with health waits
docker compose ps
docker compose logs -f tcpkv
docker compose exec postgres psql -U tcpkv -d tcpkv -c '\dt'
docker compose exec redis redis-cli ping
echo PING | nc localhost 7000
docker compose stop tcpkv; docker compose start tcpkv
docker compose down                 # stop and remove containers and network; volumes survive
docker compose down -v              # also delete volumes (data gone)
```

`depends_on` with `condition: service_healthy` makes compose wait for the health check, which
avoids the classic "server starts before the database and crashes once". `restart:
unless-stopped` is the Docker equivalent of systemd `Restart=always` (module 11). The environment
variables are how configuration gets in; the server reads them with `getenv`. Secrets (real
passwords) do not go in the compose file: use `env_file: .env` (gitignored) or Docker secrets.

### 2.8 Debugging inside a container

```bash
docker exec -it kv sh                                     # if the image has a shell
docker exec -it kv cat /proc/1/status                     # PID 1 is tcpkv
docker top kv                                             # processes, with host PIDs
docker logs --since 10m kv
docker inspect --format '{{.State.ExitCode}} {{.State.OOMKilled}}' kv
docker run --rm -it --pid=container:kv --network container:kv --cap-add SYS_PTRACE \
    tcpkv:build bash                                      # debug container sharing kv's PID and net namespaces
# inside: apt-get install -y gdb strace; strace -f -p 1; gdb -p 1
docker cp kv:/tmp/core.1 ./core.1                         # copy a file out
docker run --rm -it --cap-add SYS_PTRACE --security-opt seccomp=unconfined -p 7000:7000 tcpkv:dev
```

`--cap-add SYS_PTRACE` is required for `gdb`/`strace` inside a container because Docker drops that
capability by default; `seccomp=unconfined` is needed for some ptrace operations on older kernels.
Core dumps inside containers go where the **host's** `core_pattern` says; with `systemd-coredump`
on the host they land in the host's journal. For a reproducible debug setup, run the `build`
stage with your source bind-mounted and gdb installed.

Distroless/scratch images have no shell: the `--pid=container:` debug-sidecar pattern above is the
answer; Kubernetes calls this an ephemeral debug container.

### 2.9 Image size and hygiene checklist

- Multi-stage; runtime stage from a slim base; copy only installed artefacts.
- `--no-install-recommends`; clean apt lists in the same layer; no build tools in runtime.
- `.dockerignore` with `build/`, `.git/`, `*.md`, editor directories.
- Non-root `USER`; read-only root filesystem where possible (`docker run --read-only
  --tmpfs /tmp`).
- Pin base image tags (and digests for production); rebuild regularly for security patches.
- `HEALTHCHECK` so orchestrators know when it is really up:
  `HEALTHCHECK CMD sh -c 'printf "PING\n" | nc -w1 localhost 7000 | grep -q PONG'`.
- Labels: `LABEL org.opencontainers.image.source=https://github.com/<user>/tcpkv` and
  `org.opencontainers.image.version`.
- Scan: `docker scout cves tcpkv:0.1.0` or `trivy image tcpkv:0.1.0`.
- `docker history` and `dive` (`apt install dive` or the GitHub release) to find fat layers.

## 3. Code

- `code/tcpkv/Dockerfile`: the multi-stage build above, with comments and a `HEALTHCHECK`.
- `code/tcpkv/.dockerignore`.
- `code/tcpkv/docker-compose.yml`: tcpkv + PostgreSQL 16 + Redis 7 with health checks and volumes.
- `code/tcpkv/deploy/initdb/001_schema.sql`: a table created on first PostgreSQL start, so the
  compose stack is useful for 07_Databases_Internals.

Run it:

```bash
cd code/tcpkv
docker build -t tcpkv:dev .
docker images tcpkv:dev                                  # check the size
docker run --rm -d --name kv -p 7000:7000 tcpkv:dev
printf 'SET a 1\nGET a\nQUIT\n' | nc -q1 localhost 7000  # OK / VALUE 1
docker logs kv
docker stop kv                                           # watch "shutting down" in the logs: SIGTERM handled
docker compose up -d --build && docker compose ps && docker compose down
```

## 4. Under the hood (senior-level)

**Namespaces and cgroups.** `docker run` asks the daemon (via containerd and runc) to `clone()`
a process with `CLONE_NEWPID` (own PID numbering: your process is PID 1), `CLONE_NEWNET` (own
interfaces and port space), `CLONE_NEWNS` (own mount table), `CLONE_NEWUTS` (hostname),
`CLONE_NEWIPC`, and optionally `CLONE_NEWUSER`. It then `pivot_root`s into the assembled image
filesystem and `execve`s your entrypoint. cgroups v2 limit CPU (`cpu.max`), memory
(`memory.max`), PIDs and I/O. `ls -la /proc/self/ns/` inside and outside a container shows
different inode numbers for each namespace. There is no hypervisor; `uname -r` inside the
container is the host kernel. On Docker Desktop the "host" is the WSL2 utility VM's kernel.

**Overlay filesystem.** Image layers are directories under `/var/lib/docker/overlay2/`. OverlayFS
mounts them as lower (read-only) layers with one upper (writable) layer for the container.
Reading a file finds the topmost layer containing it; writing copies the file up to the upper
layer first (copy-on-write), which is why writing to a big file inside a container is slow the
first time and why container writes should go to volumes.

**PID 1 semantics.** In a PID namespace, PID 1 gets special treatment: signals with default
dispositions are **not delivered** to it unless it installed a handler (so a naive program
ignores `docker stop`'s SIGTERM and gets SIGKILLed after 10 s), and it must reap orphaned
children or they stay as zombies. `tini` (or `docker run --init`) is a 10 KB init that
forwards signals to your process and reaps zombies. tcpkv installs a SIGTERM handler, so it would
work without tini; the init is still good practice for anything that spawns subprocesses.

**Memory limits and the OOM killer.** `--memory 256m` sets `memory.max` on the container's
cgroup. When the cgroup's usage hits the limit and reclaim fails, the kernel's OOM killer selects
a process in that cgroup (by `oom_score`) and SIGKILLs it: exit 137, `OOMKilled: true` in
`docker inspect`. The JVM-style "the container has 256 MB but the process saw 64 GB" problem
applies to C++ too if you size caches from `sysconf(_SC_PHYS_PAGES)`: read
`/sys/fs/cgroup/memory.max` instead (module 11).

**Image format.** An image is an OCI manifest (JSON) listing a config blob (env, entrypoint,
labels) and layer blobs (tar+gzip or zstd), each addressed by sha256 digest. `docker pull`
fetches only layers you do not already have. A registry is an HTTP server for these blobs. This is
why layer ordering also affects **push/pull time**: a changed base layer means re-uploading
everything above it.

**Networking.** The default bridge is a Linux bridge `docker0`; each container gets a `veth`
pair with one end in the container namespace and one on the bridge; `iptables`/`nftables` NAT
rules implement `-p` by DNAT from the host port to the container IP. User-defined networks add an
embedded DNS server at `127.0.0.11` inside containers that resolves service names. `ss -tlnp`
on the host shows `docker-proxy` or the kernel rule holding the published port.

**Build reproducibility.** The same Dockerfile produces different images on different days
because `apt-get update` fetches the current index and `FROM ubuntu:24.04` is a moving tag. For
reproducibility pin the base digest, pin package versions (`cmake=3.28.3-1build7`) or use a snapshot
mirror, and record the resulting image digest in the release. A multi-stage build with tests
inside ensures the shipped binary passed tests in exactly the shipped environment.

## 5. In production

- **Dev/CI/prod parity.** The `build` stage image doubles as the CI build environment (module
  09: `container: tcpkv-build` or `docker build --target build`). Compiler versions can no longer
  drift between a laptop and the pipeline.
- **Logs go to stdout/stderr.** No log files inside containers (they vanish with the container and
  fill the writable layer). `docker logs`, journald, or a log shipper collects stdout. tcpkv already
  logs to stderr for this reason.
- **Graceful stop.** `docker stop` gives 10 s (`--stop-timeout` / compose `stop_grace_period`).
  A banking server draining transactions may need 60 s; set it, and make the server honour
  SIGTERM promptly (module 11).
- **Configuration via environment variables, secrets via files.** `TCPKV_PORT`, `TCPKV_PG_DSN`
  in the environment; passwords mounted as files (`/run/secrets/pg_password`) so they do not
  appear in `docker inspect` or `ps`.
- **Health checks** let orchestrators restart a hung server. A health check that only checks the
  port is listening misses a deadlocked server; have it do a real `PING`.
- **Resource limits are not optional.** Without `--memory`, one leaking container takes the host
  down. With it, the container is OOM-killed and restarted: an incident, not an outage.
- **Your Windows context.** Windows containers exist but are rare for this kind of work; the
  Linux container story is one of the strongest arguments for porting a server to Linux.
  Being able to say "I would containerise the RPC server with a multi-stage build, non-root
  user, health check and a 30 s stop grace, and run it with a memory limit" is the answer
  interviewers want.
- **3 AM:** the container restarts every 40 seconds. `docker inspect` shows `OOMKilled: true`.
  `docker stats` during startup shows memory climbing to the 256 MB limit while loading a 300 MB
  cache file. The limit was set from a guess; the fix is sizing the limit from measured RSS plus
  headroom and reading the cgroup limit in the program to size caches. Second most common:
  exit 137 with `OOMKilled: false` is the stop timeout, which means SIGTERM is not being handled.

## 6. Try it

1. **(easy)** Build `code/tcpkv` with `docker build -t tcpkv:dev .` and `docker history tcpkv:dev`.
   Done when you can name the three largest layers and say which Dockerfile line produced each.
2. **(easy)** Run the container, connect with `nc`, then `docker stop` it and read the logs. Done
   when you see the "shutting down" line and `docker inspect` shows exit code 0. Then comment out
   the `sigaction` lines in `main.cpp`, rebuild, repeat, and observe exit 137 after 10 seconds.
3. **(medium)** Change the runtime stage to `debian:12-slim` and then to
   `gcr.io/distroless/cc-debian12`. Compare image sizes with `docker images`. For distroless,
   attach a debug sidecar with `--pid=container:` and run `strace -p 1`. Done when you have three
   sizes and have run strace against the distroless container.
4. **(medium)** Add a `HEALTHCHECK` that sends `PING` and expects `PONG`. Start the container,
   watch `docker ps` show `(healthy)`. Then make the server hang (add a `sleep(1000)` in
   `serve_client`, or stop it with `docker kill -s STOP`) and watch it go `(unhealthy)`. Done when
   you can explain what an orchestrator would do with that state.
5. **(hard)** Bring up the compose stack, connect to PostgreSQL with `psql` from the host and to
   Redis with `redis-cli`, and verify `depends_on` ordering by `docker compose logs` timestamps.
   Then add a `--memory 64M` limit to the tcpkv service and run `load_test.sh` with large values
   until it is OOM-killed. Done when `docker inspect` shows `OOMKilled: true` and you can explain
   exit code 137.
6. **(hard)** Produce a fully static tcpkv (`-static`, or `-static-libstdc++ -static-libgcc` plus a
   `FROM gcr.io/distroless/static` base) and ship it in a `FROM scratch` image. Compare size with
   the Ubuntu-based image. Done when the scratch image runs and answers `PING`, and you can list
   what you gave up (no shell, no TLS roots, glibc static caveats around `getaddrinfo`).

## 7. Interview questions

**Q: Container vs virtual machine?**
A VM runs a full guest kernel on a hypervisor; a container is a host process isolated by kernel
namespaces (PID, network, mount, UTS, IPC, user) and limited by cgroups, sharing the host
kernel. Containers start in milliseconds and cost almost nothing; VMs give stronger isolation and
can run a different OS.

**Q: Why a multi-stage Dockerfile for C++?**
The build needs gcc, cmake and headers, around a gigabyte; the runtime needs the binary and
libstdc++. A build stage compiles and runs tests, and the runtime stage copies only the installed
binary onto a slim base, giving a small image with a minimal attack surface and no source or tools.

**Q: How does Docker's layer cache work and how do you order a Dockerfile for it?**
Each instruction produces a layer reused when the instruction, its parent and any copied files
are unchanged; the first change invalidates everything after it. Put slow, stable steps first:
package installation, then build-system files, then sources. Clean up inside the same `RUN`
because later deletions do not shrink earlier layers.

**Q: What happens on `docker stop`?**
SIGTERM to PID 1 inside the container, a grace period of 10 seconds by default, then SIGKILL.
PID 1 only receives SIGTERM if it installed a handler; otherwise it is killed and exits 137.
Servers must handle SIGTERM and drain within the grace period; `tini` or `--init` helps when the
process spawns children.

**Q: Why run as a non-root user in the container?**
Root inside a container is root in the kernel's eyes for anything that escapes the namespace,
such as a kernel vulnerability or an over-privileged mount. A dedicated system user limits the
damage. It also catches code that assumes it can write anywhere.

**Q: How do containers on the same compose stack find each other?**
Compose creates a user-defined bridge network; Docker's embedded DNS resolves each service name
to its container IP. tcpkv connects to `postgres:5432` and `redis:6379` by name. `localhost`
inside a container is that container only.

**Q: Where does a container's data go when it is removed?**
The writable layer is deleted with the container. Persistent data belongs in named volumes or
bind mounts; databases in compose use named volumes that survive `docker compose down` and are
removed only with `down -v`.

**Q: A container exits with code 137. What do you check?**
137 is 128+9, SIGKILL. `docker inspect` `OOMKilled` tells whether the cgroup memory limit was hit;
if false, the stop grace period expired because the process did not exit on SIGTERM. Fix the
memory sizing or the signal handling accordingly.

## 8. Recap

- Image = layers from a Dockerfile; container = process with namespaces and cgroups over an overlay filesystem; no second kernel.
- Multi-stage: build stage with gcc/cmake runs tests and `cmake --install`; runtime stage copies the binary onto a slim base.
- Cache order: apt first, CMake files, then sources; clean in the same `RUN`; `.dockerignore` build/ and .git/.
- Non-root `USER`, `tini` or `--init`, `HEALTHCHECK` with a real protocol check, pinned bases, labels.
- `docker stop` = SIGTERM, grace, SIGKILL; 137 = SIGKILL (OOM or timeout); handle SIGTERM.
- Volumes for data; bind mounts for source during development; `/mnt/c` is slow.
- User-defined networks give DNS by service name; `-p host:container` publishes ports.
- compose: services, `depends_on` with health conditions, named volumes, env for config, files for secrets.
- Debug with `exec`, `--pid=container:` sidecars, `--cap-add SYS_PTRACE`; cores follow the host's `core_pattern`.
- Size: Ubuntu+libstdc++ ~90 MB, distroless ~25 MB, scratch+static ~2 MB; trade shell and TLS roots for size.
