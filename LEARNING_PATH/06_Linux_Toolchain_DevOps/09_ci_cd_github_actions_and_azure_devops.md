# 09. CI/CD with GitHub Actions and Azure DevOps

**Time:** ~2.5 hours   **Prerequisites:** 04, 05, 06, 08

## 1. In plain words

Continuous Integration means: every time anyone pushes code, a machine you do not control checks
out that exact commit on a clean system, builds it with the project's flags, runs every test, and
reports pass or fail next to the commit. Nobody has to remember to run the tests. Nobody can say
"it builds on my machine". Continuous Delivery/Deployment extends that: if the checks pass, the
machine also produces the release artefact (a binary, a Docker image) and optionally deploys it.

For your resume, CI is where "Builds clean under -Wall -Wextra -Werror with CMake; GoogleTest
suite" becomes **verifiable**: a green badge on the README, and a workflow file anyone can read.
GitHub Actions and Azure Pipelines are the two systems you list. They are the same idea with
different YAML dialects: a trigger, one or more jobs, each job a sequence of steps run on a fresh
virtual machine or container.

Analogy: CI is a very patient junior engineer who, on every commit, re-does the full build and test
from scratch, exactly as documented, and refuses to let anyone merge until it is green.

## 2. The real thing

### 2.1 Anatomy of a pipeline

```
trigger (push, pull_request, tag, schedule, manual)
  -> workflow / pipeline
       -> job A (runs on an Ubuntu VM)      -> job B (parallel)      -> job C (needs: A, B)
            step 1: checkout                    ...                       build Docker image
            step 2: install deps                                          push on tag
            step 3: cmake --preset ...
            step 4: ctest
            step 5: upload artifacts
```

Jobs run in parallel on separate machines unless they declare dependencies. Each job starts from
a clean image; anything you need (compilers beyond the preinstalled ones, libraries) you install
in steps or by running the job inside a container. A **matrix** expands one job definition into
several (gcc and clang, Debug and Release). **Artifacts** are files uploaded from a job and
downloadable later or by other jobs. **Caches** persist directories between runs keyed by a hash
(dependency downloads, ccache). **Secrets** are encrypted variables exposed only to steps that
need them. **Status checks** are the results branch protection waits for (module 06).

### 2.2 A complete GitHub Actions workflow for tcpkv

`.github/workflows/ci.yml` (full file in `code/tcpkv/.github/workflows/ci.yml`):

```yaml
name: CI

on:
  push:
    branches: [main]
    tags: ['v*']
  pull_request:
    branches: [main]
  workflow_dispatch:

concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true

env:
  CMAKE_GENERATOR: Ninja

jobs:
  build-test:
    name: ${{ matrix.compiler }} / ${{ matrix.build_type }}
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        compiler: [gcc, clang]
        build_type: [Debug, RelWithDebInfo]
        include:
          - compiler: gcc
            cc: gcc-13
            cxx: g++-13
          - compiler: clang
            cc: clang-18
            cxx: clang++-18
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends ninja-build ${{ matrix.cc }} ${{ matrix.cxx }} ccache

      - name: Cache ccache and FetchContent
        uses: actions/cache@v4
        with:
          path: |
            ~/.cache/ccache
            build/ci/_deps
          key: ${{ runner.os }}-${{ matrix.compiler }}-${{ matrix.build_type }}-${{ hashFiles('CMakeLists.txt', 'CMakePresets.json', 'tests/CMakeLists.txt') }}
          restore-keys: |
            ${{ runner.os }}-${{ matrix.compiler }}-${{ matrix.build_type }}-

      - name: Configure
        run: >
          cmake -S . -B build/ci -G Ninja
          -DCMAKE_BUILD_TYPE=${{ matrix.build_type }}
          -DCMAKE_C_COMPILER=${{ matrix.cc }}
          -DCMAKE_CXX_COMPILER=${{ matrix.cxx }}
          -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
          -DTCPKV_WERROR=ON

      - name: Build
        run: cmake --build build/ci --parallel

      - name: Test
        run: ctest --test-dir build/ci --output-on-failure --parallel 4 --output-junit junit.xml

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: junit-${{ matrix.compiler }}-${{ matrix.build_type }}
          path: build/ci/junit.xml

      - name: Upload binary with symbols
        if: matrix.compiler == 'gcc' && matrix.build_type == 'RelWithDebInfo'
        uses: actions/upload-artifact@v4
        with:
          name: tcpkv-linux-x86_64-unstripped
          path: build/ci/tcpkv
          retention-days: 30

  sanitizers:
    name: ${{ matrix.preset }}
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        preset: [asan, tsan]
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y --no-install-recommends ninja-build
      - run: cmake --preset ${{ matrix.preset }}
      - run: cmake --build --preset ${{ matrix.preset }}
      - run: ctest --preset ${{ matrix.preset }}

  valgrind:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y --no-install-recommends ninja-build valgrind
      - run: cmake --preset debug -DTCPKV_WERROR=ON
      - run: cmake --build --preset debug
      - name: memcheck
        run: >
          valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=definite
          --track-origins=yes ./build/debug/tests/tcpkv_tests --gtest_filter='-*Concurrency*'

  docker:
    needs: [build-test, sanitizers]
    runs-on: ubuntu-24.04
    permissions:
      contents: read
      packages: write
    steps:
      - uses: actions/checkout@v4
      - uses: docker/setup-buildx-action@v3
      - name: Log in to GHCR
        if: github.event_name != 'pull_request'
        uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - name: Build (and push on main or tag)
        uses: docker/build-push-action@v6
        with:
          context: .
          push: ${{ github.event_name != 'pull_request' }}
          tags: |
            ghcr.io/${{ github.repository }}:${{ github.sha }}
            ghcr.io/${{ github.repository }}:${{ github.ref_name }}
          cache-from: type=gha
          cache-to: type=gha,mode=max
```

What each part does:

- `on`: run for pushes to `main` and version tags, for every PR targeting `main`, and by hand.
- `concurrency` with `cancel-in-progress`: pushing again to a PR cancels the stale run. Saves minutes.
- `build-test` matrix: 2 compilers x 2 build types = 4 jobs in parallel. `fail-fast: false` so a
  Clang failure does not cancel the GCC job; you want to see all failures at once. `include` adds
  per-compiler variables.
- `ubuntu-24.04` runners come with gcc-13, clang-18, cmake 3.28+, Python, Docker preinstalled.
  Pin the OS version rather than `ubuntu-latest` so a runner upgrade does not break you mid-sprint.
- `actions/cache`: the key includes a hash of the build files; `restore-keys` falls back to the
  latest cache for this compiler when the key misses. ccache turns a 3-minute compile into 20
  seconds for small diffs (typical).
- Configure explicitly rather than with a preset here, because the matrix drives compiler and
  build type; the sanitizer jobs use presets because the preset *is* the configuration.
- `ctest --output-junit` gives a report CI systems can render per test.
- `if: always()` uploads test results even when tests failed, which is when you need them.
- The unstripped `RelWithDebInfo` binary is archived for 30 days: match a production core to its
  build by commit SHA (module 07 build IDs).
- `sanitizers`: the exact presets developers use (`cmake --preset asan`), so CI and laptops agree.
- `valgrind`: a second memory checker with a different detection model (module 07). The
  concurrency tests are excluded because they take minutes under Valgrind; a nightly job runs them.
- `docker`: `needs` makes it wait for build and sanitizer jobs. On PRs it only builds (proves the
  Dockerfile works); on `main` and tags it pushes to GitHub Container Registry with the commit SHA
  and the branch or tag name. `GITHUB_TOKEN` is an automatic secret scoped to this repo;
  `permissions: packages: write` grants it push rights. `type=gha` cache stores Docker layers
  between runs.

### 2.3 Badges

In `README.md`:

```markdown
[![CI](https://github.com/<user>/tcpkv/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/<user>/tcpkv/actions/workflows/ci.yml)
```

The badge reflects the latest run on `main`. A red badge on a pinned repo is worse than no badge:
fix or remove. Add a license badge and, if you publish images, a GHCR link. Do not add twelve.

### 2.4 Branch protection with required checks

Settings -> Branches -> `main` -> Require status checks: pick `gcc / Debug`, `gcc / RelWithDebInfo`,
`clang / Debug`, `clang / RelWithDebInfo`, `asan`, `tsan`, `valgrind`. Now a PR shows a
"Merging is blocked" banner until they pass. Pair with "Require branches to be up to date" so the
tests ran against the merge result, not a stale base.

### 2.5 The Azure DevOps equivalent

`azure-pipelines.yml` (full file in `code/tcpkv/azure-pipelines.yml`):

```yaml
trigger:
  branches: { include: [main] }
  tags:     { include: ['v*'] }
pr:
  branches: { include: [main] }

variables:
  CMAKE_GENERATOR: Ninja

stages:
- stage: Build
  jobs:
  - job: BuildTest
    displayName: Build and test
    pool: { vmImage: 'ubuntu-24.04' }
    strategy:
      matrix:
        gcc_debug:     { CC: gcc-13,   CXX: g++-13,     BUILD_TYPE: Debug }
        gcc_release:   { CC: gcc-13,   CXX: g++-13,     BUILD_TYPE: RelWithDebInfo }
        clang_debug:   { CC: clang-18, CXX: clang++-18, BUILD_TYPE: Debug }
        clang_release: { CC: clang-18, CXX: clang++-18, BUILD_TYPE: RelWithDebInfo }
    steps:
    - checkout: self
    - script: sudo apt-get update && sudo apt-get install -y --no-install-recommends ninja-build ccache $(CC) $(CXX)
      displayName: Install toolchain
    - task: Cache@2
      inputs:
        key: 'ccache | "$(Agent.OS)" | $(CC) | $(BUILD_TYPE) | CMakeLists.txt | CMakePresets.json'
        restoreKeys: 'ccache | "$(Agent.OS)" | $(CC) | $(BUILD_TYPE)'
        path: $(Pipeline.Workspace)/ccache
      displayName: Cache ccache
    - script: >
        cmake -S . -B build/ci -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
        -DCMAKE_C_COMPILER=$(CC) -DCMAKE_CXX_COMPILER=$(CXX)
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DTCPKV_WERROR=ON
      displayName: Configure
      env: { CCACHE_DIR: $(Pipeline.Workspace)/ccache }
    - script: cmake --build build/ci --parallel
      displayName: Build
      env: { CCACHE_DIR: $(Pipeline.Workspace)/ccache }
    - script: ctest --test-dir build/ci --output-on-failure --parallel 4 --output-junit $(Build.ArtifactStagingDirectory)/junit.xml
      displayName: Test
    - task: PublishTestResults@2
      condition: always()
      inputs: { testResultsFormat: JUnit, testResultsFiles: '$(Build.ArtifactStagingDirectory)/junit.xml', testRunTitle: '$(CC) $(BUILD_TYPE)' }
    - task: PublishPipelineArtifact@1
      condition: and(succeeded(), eq(variables['CC'], 'gcc-13'), eq(variables['BUILD_TYPE'], 'RelWithDebInfo'))
      inputs: { targetPath: build/ci/tcpkv, artifact: tcpkv-linux-x86_64-unstripped }

  - job: Sanitizers
    pool: { vmImage: 'ubuntu-24.04' }
    strategy:
      matrix:
        asan: { PRESET: asan }
        tsan: { PRESET: tsan }
    steps:
    - checkout: self
    - script: sudo apt-get update && sudo apt-get install -y --no-install-recommends ninja-build
    - script: cmake --preset $(PRESET) && cmake --build --preset $(PRESET) && ctest --preset $(PRESET)
      displayName: $(PRESET)

- stage: Package
  dependsOn: Build
  condition: and(succeeded(), ne(variables['Build.Reason'], 'PullRequest'))
  jobs:
  - job: Docker
    pool: { vmImage: 'ubuntu-24.04' }
    steps:
    - checkout: self
    - task: Docker@2
      displayName: Build and push image
      inputs:
        containerRegistry: 'acr-service-connection'     # a Service Connection configured in Project Settings
        repository: tcpkv
        command: buildAndPush
        Dockerfile: Dockerfile
        tags: |
          $(Build.SourceVersion)
          $(Build.SourceBranchName)
```

Mapping between the two:

| Concept | GitHub Actions | Azure Pipelines |
|---|---|---|
| File | `.github/workflows/*.yml` | `azure-pipelines.yml` (any name, selected in UI) |
| Trigger | `on: push/pull_request` | `trigger:` / `pr:` |
| Grouping | `jobs:` (optionally reusable workflows) | `stages:` -> `jobs:` -> `steps:` |
| Machine | `runs-on: ubuntu-24.04` | `pool: vmImage: ubuntu-24.04` |
| Matrix | `strategy.matrix` with lists | `strategy.matrix` with named configurations |
| Shell step | `run:` | `script:` / `bash:` |
| Reusable step | `uses: org/action@v4` | `task: Name@2` |
| Variables | `${{ matrix.x }}`, `${{ env.X }}`, `$X` in shell | `$(X)` |
| Secrets | `${{ secrets.NAME }}`; repo/org settings | Variable groups, Key Vault; `$(NAME)` marked secret |
| Artifacts | `actions/upload-artifact` | `PublishPipelineArtifact@1` |
| Cache | `actions/cache` | `Cache@2` |
| Test report | upload JUnit; third-party renderers | `PublishTestResults@2` (native rendering) |
| Dependencies | `needs:` | `dependsOn:` |
| Conditions | `if:` | `condition:` |
| Registry login | `docker/login-action` with token | Service Connection |
| Branch rules | Branch protection + required checks | Branch policies + build validation |
| Self-hosted | self-hosted runners | self-hosted agents in pools |

Azure's `stages` are a first-class concept with approvals and environments (deploy to staging,
manual approval, deploy to production); GitHub has `environments` with required reviewers for
the same purpose.

### 2.6 Artifacts, secrets and safety

**Artifacts** are files produced by a run: test reports, unstripped binaries, coverage, logs. They
expire (90 days default on GitHub, configurable). Release binaries attached to a GitHub Release
(`softprops/action-gh-release` or `gh release upload`) are permanent.

**Secrets** are encrypted at rest, injected as environment variables or expressions only into the
steps that reference them, and masked in logs. Rules: never `echo` a secret (masking is best-effort);
never put secrets in the repo, in Dockerfiles (`ARG`/`ENV` end up in image history) or in
`compose.yml`; for PRs from forks, secrets are not available by design (a malicious PR could
exfiltrate them), which is why the Docker job above only pushes when the event is not a PR.
`GITHUB_TOKEN` is per-run and expires with it. Use OIDC federation to cloud providers instead of
long-lived cloud keys when deploying.

**Supply chain:** pin third-party actions to a full commit SHA (`actions/checkout@<sha>`) in
security-sensitive repos; `@v4` is a moving tag. Dependabot can keep them updated.

### 2.7 Making CI fast and useful

- `concurrency` cancellation; shallow clones (`fetch-depth: 1` is the checkout default).
- ccache or sccache; cache `_deps`; Docker layer caching with `type=gha`.
- Split long jobs: unit tests on every PR, Valgrind on the concurrency suite nightly
  (`on: schedule: cron: '0 3 * * *'`).
- `fail-fast: false` and `if: always()` for uploads: see every failure in one run.
- Enforce formatting: a job that runs `clang-format --dry-run --Werror` on changed files, and
  `clang-tidy` with `compile_commands.json`. Formatting disputes end.
- Report flaky tests, do not retry them silently. A retry step hides real races.
- Keep the pipeline under 10 minutes for PRs. Beyond that people stop waiting and start merging
  with "CI is slow, I'll fix it later".
- Reproduce CI locally: `act` runs GitHub workflows in Docker; or simply run the same
  `cmake --preset` commands, which is why presets matter.

### 2.8 Continuous deployment

Once the image is pushed, deployment is: the target pulls `ghcr.io/<user>/tcpkv:<tag>` and
restarts. Options from simple to industrial: an SSH step that runs `docker compose pull && docker
compose up -d` on a VM; a systemd unit with `ExecStartPre=docker pull` (module 11); Kubernetes
with a Deployment manifest updated by the pipeline; Azure Container Apps / App Service with
continuous deployment from ACR. Add an `environment: production` with required reviewers so a
human approves the step. Rollback is redeploying the previous tag, which is why every image is
tagged with its commit SHA.

### 2.9 Talking about CI in interviews

Have a two-minute story:

"On tcpkv I set up GitHub Actions with a gcc and clang matrix across Debug and RelWithDebInfo,
`-Werror` on, ctest with JUnit output, separate ASan+UBSan and TSan jobs using the same CMake
presets developers use locally, a Valgrind memcheck job, ccache and FetchContent caching, and a
Docker job that builds the multi-stage image and pushes to GHCR on main and tags. Branch protection
requires all of those before merge. The unstripped RelWithDebInfo binary is archived per commit for
symbolising cores. I wrote the equivalent `azure-pipelines.yml` with stages, a matrix and a Docker
task to an ACR service connection."

Then be ready for follow-ups: why both compilers (different warnings and UB detection), why
sanitizers in CI rather than locally (they always run, on every PR, on a clean machine), what you
do about flaky tests (fix or quarantine with a label, never retry), how long the pipeline takes
and how you keep it fast (cache, cancellation, splitting nightly jobs), how secrets are handled
(repo secrets, not available to fork PRs, OIDC for cloud).

## 3. Code

- `code/tcpkv/.github/workflows/ci.yml`: the complete workflow.
- `code/tcpkv/azure-pipelines.yml`: the complete Azure equivalent.
- `code/tcpkv/scripts/build.sh` and `run_tests.sh` (module 10): the pipeline calls the same
  commands a developer runs, so a CI failure is reproducible with one command locally.

Validate locally before pushing:

```bash
# YAML syntax
python3 -c 'import yaml,sys; yaml.safe_load(open(".github/workflows/ci.yml")); print("ok")'
# the exact steps of the gcc/Debug job
cmake -S . -B build/ci -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-13 -DTCPKV_WERROR=ON
cmake --build build/ci --parallel && ctest --test-dir build/ci --output-on-failure
# the sanitizer job
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
# the GitHub CLI shows runs and logs without a browser
gh run list; gh run watch; gh run view --log-failed
```

## 4. Under the hood (senior-level)

**What a runner is.** A GitHub-hosted runner is a fresh VM (2 vCPU, 7 GB RAM, 14 GB SSD for the
standard Linux runner; numbers from GitHub's documentation) created from an image with hundreds
of preinstalled tools, destroyed after the job. The runner agent polls GitHub for jobs, receives
the job definition, executes steps in order, streams logs back, and uploads artifacts. `uses:`
actions are Git repositories checked out into the runner and executed as JavaScript (Node) or as
a Docker container or as composite steps; `run:` steps spawn `bash -e {0}` by default on Linux,
so a failing command fails the step. Azure agents work the same way; the `vmImage` pool is
Microsoft-hosted, or you register your own machines.

**Why caches are keyed by hash.** The cache action tars the listed paths and stores them under the
key. On restore, an exact key hit gives the tarball; `restore-keys` prefixes give the most recent
partial match. Cache entries are immutable once saved: if the key exists, nothing is saved, which
is why the key must change when the inputs change (`hashFiles(...)`). ccache's own hashing of
compiler, flags and preprocessed source means a stale ccache directory is harmless, so the prefix
fallback is safe.

**Status checks and the merge queue.** Each job reports a check run with a name to the commit's
check suite through the Checks API; branch protection lists check names. Renaming a job breaks
the protection rule silently (the old name never reports, so merging is blocked forever) and is
a classic trap. GitHub's merge queue re-runs required checks on the speculative merge commit so
`main` never gets a combination that was not tested.

**Containers as job environments.** `container: ghcr.io/<user>/tcpkv-build:latest` runs all
steps of a job inside that image: your exact compiler, no apt steps, faster, and identical to the
Dockerfile build stage. Service containers (`services: postgres: image: postgres:16`) start
side-car containers reachable by hostname for integration tests, the CI version of compose.

**Secrets masking** works by registering the secret value with the log processor, which replaces
occurrences with `***`. Encoding the value (base64, splitting across lines) defeats it. The real
protections are that secrets are not exposed to fork PRs and that the token is short-lived. OIDC
tokens are JWTs signed by GitHub with claims about repo and branch, which the cloud provider trusts
instead of a stored key.

**Docker layer cache in CI.** Without `cache-from`, every CI Docker build starts from zero. The
`type=gha` backend exports layer blobs to GitHub's cache service using BuildKit's cache manifests;
`mode=max` exports intermediate stages (the build stage) too. A code change then rebuilds only the
layers below the `COPY src/` line, exactly as locally.

## 5. In production

- **CI is the executable definition of "done".** If a quality rule is not enforced in CI, it does
  not exist. `-Werror`, tests, sanitizers, formatting, license headers: all enforced or all
  optional.
- **Match CI to production.** Build the release artefact in CI from the tagged commit, record
  the image digest, deploy that digest. Never build on the production machine; never deploy
  something CI did not build.
- **Pipelines fail for environmental reasons** (network, runner image update, flaky third-party
  download). Pin runner OS versions, pin action versions, mirror or vendor dependencies, and
  retry *downloads* (not tests).
- **Nightly jobs catch what PR jobs cannot afford**: full Valgrind, long TSan runs, fuzzing,
  `-O3 -march=native` builds, a build against the latest compiler to see tomorrow's warnings.
- **Your Azure DevOps experience is directly relevant.** If your employer builds the RPC server
  with MSBuild in an Azure Pipeline, the structure (trigger, agent pool, `VSBuild@1` or
  `MSBuild@1` task, `VSTest@2`, publish artifact) is what section 2.5 shows with Linux tasks.
  Say that in interviews: you have seen pipelines for a real product and can write them for Linux.
- **3 AM:** the release pipeline is green but production crashes at startup with "cannot open
  shared object file". The CI build job and the Docker runtime stage drifted: someone added
  `libpq` to the build but not to the runtime image, and the tests ran in the build stage where
  the library exists. Fix: run the smoke test (`PING`) against the final image in the Docker job,
  not against the build stage. The `HEALTHCHECK` plus a `docker run` step in CI would have caught it.

## 6. Try it

1. **(easy)** Push `code/tcpkv` to a new GitHub repository with the workflow. Done when all jobs
   are green and the badge in the README renders. Record how long the pipeline took.
2. **(easy)** Open a PR that introduces an unused variable. Done when the PR is blocked by the
   `-Werror` failure and you can find the error line in the job log with `gh run view --log-failed`.
3. **(medium)** Add a `format` job that runs `clang-format --dry-run --Werror` over `src include
   tests` and a `tidy` job that runs `clang-tidy` with `-p build/ci`. Add a `.clang-format` and a
   `.clang-tidy`. Done when both pass on `main` and fail on a deliberately misformatted PR.
4. **(medium)** Add a smoke test step to the `docker` job: run the built image, send `PING` with
   `nc`, assert `PONG`, and stop it. Done when the step fails if you break the runtime stage
   (remove `libstdc++6`) and passes otherwise.
5. **(hard)** Add a `nightly` workflow on a cron schedule running the full Valgrind suite including
   concurrency tests, plus a build with `-O3 -flto`. Add a `release` job that, on a `v*` tag,
   creates a GitHub Release with the stripped binary and the unstripped symbols archive attached,
   and tags the Docker image `latest` and `<semver>`. Done when pushing `v0.1.0` produces a
   release page with two assets and `docker pull ghcr.io/<user>/tcpkv:0.1.0` works.
6. **(hard)** Port the complete workflow to Azure DevOps: create a free organisation and project,
   connect the GitHub repo, add the `azure-pipelines.yml`, create a Container Registry service
   connection (or skip the push with a condition), and get the stages green. Done when
   `PublishTestResults` shows per-test results in the Azure UI and you can explain two
   differences between the YAML dialects from memory.

## 7. Interview questions

**Q: What is CI and what does your pipeline do?**
CI builds and tests every commit on a clean machine so integration problems surface immediately.
Mine runs a gcc/clang x Debug/RelWithDebInfo matrix with `-Werror`, ctest with JUnit output,
ASan+UBSan and TSan jobs via CMake presets, a Valgrind job, caches with ccache, archives the
symbolised binary, and builds and pushes a multi-stage Docker image on main and tags. Branch
protection requires all of it before merge.

**Q: Why build with both GCC and Clang?**
They diagnose different things: different warnings, different UB exploitation, different
standard-library implementations if you switch to libc++. Code that compiles cleanly under both
is more portable and has fewer latent bugs, and you get MSan and better `-ftime-trace` from Clang.

**Q: Why run sanitizers in CI if developers can run them locally?**
Because "can" is not "do". CI runs them on every PR, on a clean machine, with the same preset,
and blocks the merge. A sanitizer finding in CI is attached to the exact commit; locally it is
a terminal nobody saw.

**Q: How do you handle secrets in a pipeline?**
Stored as encrypted repository or organisation secrets, exposed only to steps that reference
them, masked in logs, unavailable to pull requests from forks. Prefer short-lived tokens:
`GITHUB_TOKEN` for the registry, OIDC federation for cloud providers instead of stored keys.
Never bake secrets into Docker images or print them.

**Q: What is a matrix build?**
One job definition expanded over combinations of variables (compiler, build type, OS) into
parallel jobs. `fail-fast: false` keeps the others running when one fails so all problems are
visible in one run.

**Q: How do you keep CI fast?**
Cancel superseded runs, cache compiler output with ccache and dependency downloads by content
hash, cache Docker layers, run expensive suites nightly instead of per PR, and parallelise with a
matrix and `ctest -j`. Target under ten minutes for PR feedback.

**Q: GitHub Actions vs Azure Pipelines: main differences?**
Same model: triggers, jobs on hosted or self-hosted machines, steps of scripts and reusable
tasks, matrices, artifacts, caches, secrets. Azure has first-class stages with approvals and
environments and native test-result rendering; GitHub has a larger marketplace of actions,
environments with reviewers, and tighter integration with PRs and GHCR. Syntax differs:
`${{ }}` vs `$( )`, `uses:` vs `task:`, `needs:` vs `dependsOn:`.

**Q: A CI job passes but the deployed container crashes. How do you prevent that class of bug?**
Test the artefact you ship: run the final image in the pipeline and execute a smoke test against
it; add a `HEALTHCHECK`; build the runtime stage's dependency list from `ldd` of the binary;
deploy by image digest so what was tested is what runs.

## 8. Recap

- CI: every commit built and tested on a clean machine; results block merging via required checks.
- Workflow = triggers + jobs (parallel VMs) + steps; matrix expands jobs; `needs`/`dependsOn` orders them.
- tcpkv pipeline: gcc/clang x Debug/RelWithDebInfo with `-Werror`, ctest JUnit, asan/tsan presets, Valgrind, ccache cache, symbol artifact, Docker build and push.
- Pin runner OS and action versions; `fail-fast: false`; `if: always()` for result uploads; cancel superseded runs.
- Caches keyed by content hash with prefix fallbacks; Docker layer cache with `type=gha`.
- Secrets: encrypted, masked, not available to fork PRs; `GITHUB_TOKEN` and OIDC over long-lived keys.
- Azure Pipelines: `trigger`/`pr`, `stages` -> `jobs` -> `steps`, `$(VAR)`, `task:`, `Cache@2`, `PublishTestResults@2`, service connections.
- Badges on the README; branch protection requires the named checks; renaming a job breaks the rule.
- Smoke-test the final image in CI; deploy by digest; nightly jobs for slow checks.
- Two-minute interview story ready, with follow-up answers on compilers, sanitizers, flakes, speed, secrets.
