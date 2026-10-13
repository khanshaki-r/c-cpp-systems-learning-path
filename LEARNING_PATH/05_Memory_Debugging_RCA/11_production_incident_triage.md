# 11. Production incident triage

**Time:** ~3 hours   **Prerequisites:** 06, 07, 09, 10

## 1. In plain words

An incident is the period between "something is wrong in production" and "it is fixed and we
understand why". Triage is the first part of it: deciding how bad it is, stopping the damage, and
collecting enough evidence that you can find the cause later. Debugging is what you do afterwards,
calmly, with the evidence on your desk.

The analogy is an emergency room. The doctor at the door does not start surgery. They check
breathing and bleeding, decide who is most urgent, stabilise the patient, and draw blood *before*
giving drugs that would change the test results. A server is the same. The dump, the logs and the
metrics are the blood sample. Restart the process first and the sample is gone; the client is
back up, but you will never know why it fell over, and it will fall over again next Tuesday.

The skill this module teaches is doing the first 15 minutes in the right order, every time, at
3 AM, with a bank's operations manager on the phone. Most of it is decided before the incident:
what the server logs, whether dumps are configured, whether there is a feature flag to switch the
broken path off, and whether operations knows how to take a dump without calling you.

## 2. The real thing

### 2.1 The first 15 minutes, in order

The order matters more than speed. Each step takes a minute or two; skipping one costs days.

| Minute | Action | Why in this order |
|--------|--------|-------------------|
| 0-2 | **Acknowledge** in the incident channel / to the client contact. "Investigating, next update at HH:MM." Open an incident log (a text file with timestamps is enough). | The client's first fear is silence. The log becomes the RCA timeline. |
| 2-4 | **Scope**: what is broken, for whom, since when. One branch or all? One API or all? Errors, hangs or wrong results? | Decides severity and whether this is your component at all. |
| 4-6 | **Classify severity** (2.2) and escalate if Sev-1. | Severity decides who wakes up and how often you report. |
| 6-10 | **Capture evidence** while the faulty state still exists: dump of the hung or looping process, logs around the start time, metrics snapshot, config and build version. | After the restart the state is gone. A hung process is the evidence. |
| 10-13 | **Stop the bleeding**: restart, fail over, roll back, switch a feature flag off, block one bad input. Pick the smallest action that restores service (2.4). | Mitigation before root cause. The client cares about the service first. |
| 13-15 | **Verify recovery** with a real signal (transactions succeeding, error rate back to baseline), then send the second update. | "We restarted it" is not recovery. Success rate is. |

After minute 15 the incident becomes an investigation: you work on the evidence copy, not on the
production box, and you keep sending updates on the agreed cadence until it is resolved.

### 2.2 Severity classification

Severity is about business impact, not technical interest. A beautiful heap corruption in a
nightly report job is lower severity than a boring config typo that stops UPI payments.

A typical four-level scheme (the exact names and response times come from the support contract
with the client, so read yours):

| Sev | Meaning | Example in core banking | Typical response |
|-----|---------|-------------------------|------------------|
| 1 | Critical business function down or money at risk, no workaround | Fund transfer service crashing in a loop; wrong balances being posted | Immediate, all hands, updates every 30 min, client leadership informed |
| 2 | Major function degraded, workaround exists or partial outage | One branch cluster down, others serving; latency 10x but completing | Within an hour, updates every 1-2 hours |
| 3 | Minor function affected, low business impact | Statement PDF generation failing for some accounts; retry works | Next business day |
| 4 | Cosmetic, question, or no current impact | Log message wrong; a single crash that self-recovered | Planned release |

Three rules that matter more than the table:

- **Data integrity beats availability.** A service that is up but posting wrong amounts is worse
  than a service that is down. If you suspect wrong results, the mitigation may be to *stop* the
  service. Say so explicitly to the client.
- **Severity can go up or down.** Re-evaluate at every update. Downgrading after mitigation is
  normal; say why.
- **Repeated low-severity crashes are a high-severity problem.** A process that crashes once a day
  and auto-restarts is "only Sev-3" until the day it crashes during end-of-day batch.

### 2.3 Capturing evidence

What to capture depends on the failure mode. Do the capture as a script that operations can run,
not a sequence of commands typed by a tired person (`code/m11_collect_evidence.sh`).

| Failure mode | Capture on Linux | Capture on Windows |
|--------------|------------------|--------------------|
| Crashed | The core (`coredumpctl dump PID -o file` or from `core_pattern`), the exact binary, its `.debug` file, the shared libraries (`NT_FILE` list), logs | The `.dmp` from WER LocalDumps or ProcDump `-e`, the exe, the PDBs, logs |
| Hung / deadlocked | `gcore -o hang PID` twice, 30-60 s apart, then logs, then restart | `procdump -ma PID` twice, or Task Manager "Create dump file" |
| High CPU / spinning | `gcore` twice plus `top -H -p PID` (per-thread CPU) and `perf top -p PID` for 10 s | ProcDump `-c 90 -s 10 -n 2` (dump when CPU > 90% for 10 s), Process Explorer thread view |
| Memory growth | RSS history from metrics, `/proc/PID/smaps_rollup`, one `gcore` (big, but valuable), heaptrack attach if allowed | Two `procdump -ma` hours apart for `!heap -s` comparison, UMDH snapshots if `+ust` is on |
| Wrong results | Logs with request ids for the affected transactions, the input data, the DB rows; the process may be fine | Same |

Always also capture: build version and build id (`readelf -n binary | grep Build`), the deployed
config, the host (`uname -a`, distro, glibc version), the time window, and what changed recently
(deploys, config pushes, OS patches, traffic). "What changed?" answers more incidents than any
debugger.

**Two dumps of a hang.** One dump of a hung process tells you where every thread is. Two dumps a
minute apart tell you whether anything is moving: identical stacks mean deadlock; same threads in
different frames mean livelock or a very slow loop; everything different means it is just slow
(module 10).

**Dump size and data protection.** A full dump of a banking server contains customer data:
account numbers, names, maybe card data and session tokens. Treat it like production data. It
stays inside the client's environment or moves through the approved secure channel, is stored
encrypted, has access control, and is deleted when the investigation closes. Never attach a dump
to an email or a public ticket. If the contract does not allow dumps to leave the client site,
you analyse on their jump host. Know this *before* the incident.

### 2.4 Stopping the bleeding: restart versus keep for forensics

This is the central decision. It is a trade-off between service and evidence, and the answer is
almost always "both, in that order, within two minutes".

```
            is the process still alive and in the faulty state?
                     |                         |
                    yes                        no (crashed)
                     |                         |
     take dump(s) NOW (60-120 s)       is the core / .dmp there?  -> if not, fix dump config
                     |                         |                     as part of the incident
     is there a standby / other node?  restart (supervisor already did?)
           |                 |                 |
          yes               no                 is it crash-looping?
           |                 |                 |              |
   fail over first,    restart after          no             yes -> roll back / flag off /
   then keep the bad    the dump              done                  block the bad input
   node isolated for
   live debugging
```

Mitigation options from smallest blast radius to largest:

1. **Block the bad input.** If one malformed message or one account triggers the crash, reject it
   at the edge (a deny-list, a validation rule). The service stays up for everyone else.
2. **Switch off a feature flag.** If the crash is in a new code path, turn it off. This is why
   new paths should ship behind flags (2.7).
3. **Fail over** to the standby node, leaving the faulty node isolated. Best of both worlds: the
   service is restored and the evidence is still running.
4. **Restart** the process. Loses in-memory state; check that in-flight transactions are either
   completed or safely rolled back (idempotency, 2.7).
5. **Roll back** the last deployment. Effective if the incident started with a deploy; costs the
   new features.
6. **Stop the service.** Correct when results are wrong and money is at risk.

When the client pushes to "just restart it, now", the answer is: "Yes. I need 90 seconds to take
a memory snapshot first, otherwise we cannot find the cause and it will happen again." In a
banking context this is almost always accepted if you say it calmly and the 90 seconds are real.
Having the capture as a one-line script is what makes it 90 seconds.

When you do *not* wait: when every second of the faulty state is posting wrong money, or a crash
loop restarts faster than you can type (then the cores are being written anyway, check the
`core_pattern` and disk space).

### 2.5 Communication cadence with a bank client

The bank's operations and IT leadership have their own escalation chain and their own regulator.
Your updates are what they forward upwards. Make them easy to forward.

Rules:

- **Fixed cadence, announced in advance.** "Next update at 03:30." Then update at 03:30 even if
  there is no news. "No change; still analysing the dump; next update 04:00" is a valid update.
  Silence is what turns a Sev-2 into an escalation to your CEO.
- **One voice.** One person (the incident commander or the support lead) talks to the client.
  Engineers feed facts to that person. Five engineers posting guesses in the client's channel
  destroys confidence.
- **Facts, actions, next time.** Every update has: current status, what we know, what we are
  doing, what we need from you, next update time.
- **Separate fact from hypothesis.** "The process crashed at 02:41 with an access violation"
  is fact. "We believe it is related to yesterday's release" is a hypothesis; label it.
- **Never promise a root cause time.** Promise the next update time. "Root cause by 10:00" is a
  promise you cannot keep; "preliminary findings by 10:00" you can.
- **No blame, no internal names.** Not "the developer forgot a lock". "A defect in the session
  handling code."

A template update:

```
[Update 3 - 03:30 IST] Fund Transfer Service - Sev-1

Status:       Service restored at 03:12 IST via restart of APP-NODE-2. Transactions succeeding
              at normal rate since 03:14 (monitoring confirms).
Impact:       Between 02:41 and 03:12 IST fund transfer requests on APP-NODE-2 failed. Node 1
              continued serving. Approx. count of affected requests will be confirmed from logs.
Known so far: The service process terminated unexpectedly at 02:41. A memory dump was captured
              before restart and is under analysis.
Doing now:    Analysing the dump; checking logs for the request pattern before the failure.
Need from you: Confirmation that no batch job was started on APP-NODE-2 around 02:30.
Next update:  04:30 IST, or earlier if status changes.
```

After resolution: a short closure note the same day ("resolved, monitoring, RCA by <date>"), then
the formal RCA within the contractual window (module 12). Typical contracts ask for a preliminary
RCA within 24-72 hours and a final one within 5-10 business days; check yours.

### 2.6 What a server should log by design

You cannot add logs during an incident. Whatever the server logs today is all you will have. Design
the logging for the 3 AM reader.

**Every log line carries:**

| Field | Example | Why |
|-------|---------|-----|
| Timestamp, UTC or with offset, millisecond or microsecond resolution | `2026-10-12T02:41:07.318+05:30` | Correlate with other systems; order events within a second |
| Level | `ERROR` | Filter |
| Process id and thread id (kernel tid, not the `pthread_t`) | `pid=4120 tid=4187` | Match a log line to a thread in the core (`info threads` shows LWP) |
| Request id / correlation id | `req=7f3a2c19-...` | Follow one transaction across threads, processes and services |
| Component / module | `comp=xfer` | Find the area |
| Event name, stable | `evt=xfer.debit.failed` | Grep and count without parsing prose |
| Key fields | `acct=XXXX1234 amt=5000 err=DB_TIMEOUT` | The facts, masked where needed |
| Build version | at startup and in the crash handler | Match the dump to the binary |

**Structured logs.** Write `key=value` pairs or one JSON object per line, not free text. Free text
like `"Failed to debit account 1234 because timeout"` cannot be counted or joined reliably. A
structured line can (one line in the file; wrapped here to fit):

```
ts=2026-10-12T02:41:07.318+05:30 lvl=ERROR pid=4120 tid=4187 req=7f3a2c19 comp=xfer
  evt=xfer.debit.failed acct=XXXX1234 amt_paise=500000 err=DB_TIMEOUT latency_ms=5003 build=4.12.3
```

**What else to log:**

- **Startup**: build version and id, config hash, host, glibc version, limits (`ulimit -c`!),
  thread pool sizes. Half the RCAs start with "which version was running?".
- **Request boundaries**: start and end of every request with duration and result. These two
  lines alone let you find the last request a crashed thread was handling (start without end).
- **State transitions**: connection open/close, session create/expire, failover, config reload.
- **Resource gauges** every minute: RSS, heap in use (from `mallinfo2`), thread count, fd count,
  queue depths, pool usage. These turn "memory growth" into a slope you can read (module 09).
- **The crash itself**: a crash handler that writes the signal, fault address, build, thread id,
  current request id and a backtrace to stderr, then re-raises so the core is still written
  (section 3).

**What never to log**: full card numbers, PINs, passwords, CVV, OTPs, full session tokens. Mask
account numbers to the last 4 digits unless the client's policy says otherwise. Logs are copied to
more places than dumps; a leak through logs is a reportable data breach for the bank.

**Cost and volume.** Logging is I/O. Use asynchronous logging (a queue and a writer thread) so a
slow disk does not block request threads; bound the queue and *count* dropped lines rather than
blocking. Keep `DEBUG` off in production but switchable at runtime per component, without restart.

### 2.7 Log correlation

Correlation is joining events from different sources into one timeline.

- **Request id propagation.** Generate an id at the edge (or accept the client's), put it in a
  thread-local when a worker picks up the request, include it in every log line, pass it to
  downstream calls (RPC header, DB `application_name` or a comment in the SQL), and return it to
  the caller in error responses. Then "customer complaint about transfer at 10:42" becomes one
  grep.
- **Thread id to core.** The crash handler prints the tid; `info threads` in gdb shows `LWP`;
  `~` in WinDbg shows `Id: pid.tid`. The log lines with that tid just before the crash are the
  request that killed the process.
- **Clock alignment.** All servers on NTP/chrony. A 3-second skew between the app server and the DB
  server makes a timeline lie. Note the timezone in every timestamp.
- **Across a restart.** Log a "process start, previous exit reason" line. On Linux,
  `systemctl status` / `journalctl -u svc` show `code=dumped, status=11/SEGV`; on Windows the
  Application event log has event 1000 (Application Error) with the faulting module and offset.
  Correlate with the dump timestamp.

A one-liner that answers "what was each thread doing just before the crash":

```bash
# last request each thread started but never finished, before 02:41:07
awk '$0 < "ts=2026-10-12T02:41:08"' app.log | grep -E 'evt=req\.(start|end)' |
  awk '{for(i=1;i<=NF;i++){split($i,kv,"="); f[kv[1]]=kv[2]}
        if (f["evt"]=="req.start") open[f["tid"]]=f["req"]; else delete open[f["tid"]]}
       END {for (t in open) print "tid", t, "was handling", open[t]}'
```

### 2.8 Feature flags and safe restarts

**Feature flags** are runtime switches that turn a code path on or off without a deploy. For
incident response they are the cheapest mitigation there is. Rules that make them useful:

- New risky paths (a new parser, a new caching layer, a new batch algorithm) ship behind a flag,
  default off, enabled gradually.
- The flag is read at a safe point (per request), not once at startup, so flipping it does not
  need a restart.
- The old path stays working until the new one has been on for a full business cycle (month-end
  included, for a bank).
- Flags are removed once the new path is proven. A server with 200 permanent flags has 2^200
  configurations nobody has tested.

**Safe restarts** mean a restart cannot corrupt data or lose money:

- **Idempotent operations.** Every money-moving request has a unique transaction reference;
  repeating it is detected and returns the original result instead of posting twice. Then a
  client retry after a crash is safe.
- **Transactions own the truth.** The DB transaction commits or it does not; in-memory state can
  be rebuilt from the DB on startup. If a crash mid-request can leave "debited but not credited",
  that is a design bug, and the RCA should say so.
- **Graceful shutdown path**: stop accepting, drain in-flight requests with a timeout, then exit.
  The supervisor (systemd `Restart=on-failure`, a Windows service recovery action) restarts
  crashed processes; add a **restart limit** (`StartLimitBurst`) so a crash loop stops and
  alerts instead of spinning silently and filling the disk with cores.
- **Rolling restarts** across nodes, one at a time, with health checks between them.

### 2.9 Postmortem culture

A postmortem is the meeting and document after the incident where you work out what happened and
what to change. The RCA you send the bank (module 12) is its external, formal form.

- **Blameless.** The question is "how did our system allow this?", not "who did this?". People
  hide facts from a blame meeting; you need the facts. A missing lock is a review-process gap and a
  missing TSan job, not a stupid developer.
- **Timeline first, from evidence.** Logs, dump timestamps, chat history. Not memory.
- **Action items are owned, dated and verifiable.** "Improve testing" is not an action item.
  "Add TSan CI job for the session module - owner: Shakir - by 2026-10-30 - done when the job runs
  on every merge request" is.
- **Track them to closure.** Most organisations are good at writing action items and bad at
  finishing them. Review open items weekly.
- **Look for repeats.** Group incidents by stack signature (`!analyze` bucket, or a hash of the
  top 5 frames). Three incidents with the same signature in a quarter means the previous RCAs
  fixed symptoms.

## 3. Code

### 3.1 A crash handler that preserves the core

`code/m11_crash_handler.cpp` installs a handler for `SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGFPE` and
`SIGILL` that prints the signal, fault address, build id, kernel thread id, current request id and
a backtrace, then re-raises the signal with the default action so the kernel still writes the
core.

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O1 -fno-omit-frame-pointer -rdynamic \
    -o build/m11_crash_handler m11_crash_handler.cpp
ulimit -c unlimited
./build/m11_crash_handler
# *** FATAL signal 11 (SIGSEGV) fault addr 0x0000000000000010
# build  = 2026.10.03-rel-1
# thread = 12345  request = REQ-00042
# backtrace:
# ./build/m11_crash_handler(+0x...) ...
# Segmentation fault (core dumped)
```

The key parts and why they look like that:

```cpp
static thread_local char t_request_id[32] = "none";   // set by the dispatcher per request

static void crash_handler(int sig, siginfo_t *info, void *)
{
    write_str("\n*** FATAL signal "); write_dec(sig);  // write(2) only: printf takes a lock
    ...
    write_str("  request = "); write_str(t_request_id); // the log-correlation key
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);                 // writes to fd 2 without malloc
    signal(sig, SIG_DFL);                               // restore default action...
    raise(sig);                                         // ...so the kernel writes the core
}

static void install_crash_handler()
{
    static char altstack[64 * 1024];                    // stack overflow needs its own stack
    stack_t ss{}; ss.ss_sp = altstack; ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    void *warm[4]; backtrace(warm, 4);                  // first call may malloc: do it now
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    ...
}
```

Why each rule exists:

- **Async-signal-safe calls only.** The handler runs on the crashed thread, in the middle of
  whatever it was doing. If it crashed inside `malloc` (heap corruption, module 03), the arena lock
  is held; calling `malloc` or `printf` (which may allocate and takes the stdio lock) from the
  handler deadlocks the process. Now you have a hang instead of a crash, and *no core*. The POSIX
  list of safe functions is in `man 7 signal-safety`: `write`, `_exit`, `sigaction`, `raise`, and
  not much more. Hence hand-written number formatting.
- **`sigaltstack` + `SA_ONSTACK`.** For a stack overflow the faulting thread has no stack left;
  without an alternate stack the handler itself faults and the kernel kills the process.
  (The alternate stack is per thread; a real server installs one in each thread it creates.)
- **Warm up `backtrace()`.** glibc's `backtrace` loads `libgcc_s` via `dlopen` on first use, which
  allocates. Calling it once at startup moves that out of the handler.
- **Re-raise with `SIG_DFL`.** If the handler just returned or called `_exit`, there would be no
  core. `SA_RESETHAND` plus `raise` gives the default action, which is "terminate and dump".
- **The request id is a `thread_local` char array**, not a `std::string`: reading it from the
  handler must not touch the heap.

On Windows the equivalent is a vectored exception handler or `SetUnhandledExceptionFilter` that
writes a short line and calls `MiniDumpWriteDump` from a *separate* watchdog process or thread
(writing a dump from the crashed thread itself is unreliable for the same reasons). In practice
WER LocalDumps or ProcDump `-e` is simpler and more robust; configure them (module 07).

### 3.2 Evidence collection script

`code/m11_collect_evidence.sh` is the script operations runs at minute 6. It takes a PID (hung
process) or a core path (crashed process) and produces one tarball with everything module 06 says
a "complete core" needs.

```bash
sudo ./m11_collect_evidence.sh --pid 4120 --log /var/log/app/app.log --out /secure/incidents
# [collect] gcore 1 of 2 ...
# [collect] waiting 30 s for second gcore (hang analysis needs two)
# [collect] gcore 2 of 2 ...
# [collect] per-thread CPU, limits, maps, fds, binary, debug link, shared libs, last 20000 log lines
# [collect] wrote /secure/incidents/inc-20261012T024312-pid4120.tar.gz (sha256 ...)
```

Read the script once: it is mostly a checklist turned into commands. It records the SHA-256 of the
tarball so the RCA can say which evidence was analysed (chain of custody).

### 3.3 A structured log line in C++

Not a separate file; the core of a logger you would add to your server. It shows the fields from
2.6 and the thread-local request id shared with the crash handler.

```cpp
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

thread_local char t_request_id[32] = "none";          // same variable the crash handler reads

inline long current_tid() { return static_cast<long>(syscall(SYS_gettid)); }

// One line, key=value, UTC with microseconds. In a real server this formats into a buffer and
// pushes it to an async writer thread; snprintf + one fwrite keeps lines from interleaving.
inline void log_event(const char *lvl, const char *comp, const char *evt, const char *kv)
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto us  = duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);                                  // reentrant; gmtime is not thread-safe
    char line[512];
    int n = std::snprintf(line, sizeof line,
        "ts=%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ lvl=%s pid=%d tid=%ld req=%s comp=%s evt=%s %s\n",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long>(us), lvl, static_cast<int>(getpid()), current_tid(), t_request_id,
        comp, evt, kv);
    if (n > 0) std::fwrite(line, 1, static_cast<size_t>(n) < sizeof line ? static_cast<size_t>(n)
                                                                          : sizeof line - 1, stderr);
}

// usage at a request boundary:
//   std::snprintf(t_request_id, sizeof t_request_id, "%s", req.id);
//   log_event("INFO", "xfer", "req.start", "acct=XXXX1234 amt_paise=500000");
```

## 4. Under the hood (senior-level)

**What the kernel does when a process crashes.** The faulting instruction raises a CPU exception;
the kernel's page-fault handler finds no valid mapping and queues `SIGSEGV` to the thread. If a
handler is installed, the kernel builds a signal frame on the (alternate) stack and resumes the
thread in the handler. When the default action runs, the kernel calls `do_coredump`: it checks
`RLIMIT_CORE` (`ulimit -c`), the dumpable flag (`prctl(PR_SET_DUMPABLE)`, cleared by `setuid`
transitions), then either writes the file named by `core_pattern` or, if the pattern starts with
`|`, pipes the core to a user-space program (`systemd-coredump`, `apport`). Only then does the
process die and its parent (systemd) get `SIGCHLD` with "dumped core". So a misconfigured
`core_pattern`, a full disk, `ulimit -c 0` in the service unit (`LimitCORE=` in systemd) or a
`setuid` binary each silently produce no core. Check these at startup and log them.

**Why `gcore` pauses the service.** `gcore` attaches with `ptrace`, which stops all threads, then
reads every writable mapping through `/proc/PID/mem` and writes the ELF core. For a 20 GB process
that is tens of seconds of a frozen server (disk speed bound). Health checks may fail and a load
balancer may take the node out - which is fine during a hang, but plan for it. On Windows
`procdump -r` uses process reflection (clones the process and dumps the clone) to cut the pause.

**Windows Error Reporting.** On an unhandled exception, `kernel32!UnhandledExceptionFilter` hands
the process to WER (`WerFault.exe`), which suspends it and, if `HKLM\SOFTWARE\Microsoft\Windows\
Windows Error Reporting\LocalDumps` is configured (`DumpType=2` for full), writes the dump. The
Application event log entry (Event ID 1000) records the faulting module, its version and the
offset - enough to correlate without opening the dump. A service with "recovery: restart" in the
SCM restarts after WER finishes.

**Log ordering across threads is not causal order.** Two threads that log within the same
microsecond can appear in either order; with an async logger, the queue order is the enqueue
order, which is not the order the events happened. Within one thread, order is reliable. Across
threads, only synchronisation (a lock both took, a message passed) establishes order. That is why
the request id plus tid matter more than the timestamp.

**`syscall(SYS_gettid)` versus `pthread_self()`.** `pthread_self()` returns the address of the
thread's `struct pthread` (its TCB) in user space; it is what gdb shows as `Thread 0x7f...`. The
kernel tid (LWP) is what `top -H`, `/proc/PID/task/`, `perf` and the crash handler use. Log the
kernel tid; it is the one that joins logs, `top` and the core. (glibc 2.30+ has `gettid()`.)

## 5. In production

- **Test the dump path before you need it.** After every deployment or OS patch, on a staging box
  with the same config, `kill -SEGV` a test process (or the real one in a maintenance window) and
  confirm the core appears with the right size and opens in gdb with symbols. Dump configuration
  silently breaks: a systemd unit file update resets `LimitCORE`, a disk cleanup cron deletes
  `/var/lib/systemd/coredump`, someone sets `DumpType=1` (minidump) to save space.
- **Store symbols for every release.** The `.debug` files / PDBs for every build that ever reached
  a client, indexed by build id, kept for the life of the support contract. A dump without matching
  symbols costs a day; a dump of a build whose symbols were deleted is nearly useless.
- **The 3 AM checklist is printed.** At 3 AM nobody remembers `procdump -ma`. Keep a one-page
  runbook per service: where the logs are, how to take a dump, how to fail over, how to flip the
  flags, who to call.
- **Watch for the restart that hides the problem.** A supervisor that silently restarts a crashing
  process makes availability look fine while the system crashes 40 times a day. Alert on restarts,
  not only on downtime.
- **Disk full during the incident.** Cores of a big process can fill `/var` and take down logging
  and the DB on the same box. Put cores on a separate volume with a quota and a retention policy.
- **Banking specifics.** End-of-day and month-end batches are the high-risk windows; freeze
  deploys around them. Regulators may require incident reporting within hours for customer-facing
  outages; your RCA timeline will be used in that report, so the timestamps must be exact and the
  timezone stated.
- **War story pattern.** The crash is in the overnight interest calculation; operations restarts it
  three times before calling anyone; each restart partially re-runs the batch. The incident is now
  a data reconciliation incident, much bigger than the crash. The fix in the runbook: "batch job
  crashes - do not restart - take the core - call the on-call engineer". Restarts are not always
  safe; the runbook must say which ones are.

## 6. Try it

1. **Easy - core configuration audit.** On WSL, write down the current values of `ulimit -c`,
   `/proc/sys/kernel/core_pattern`, and (if systemd) `coredumpctl list`. Crash
   `m11_crash_handler` and find the core. Done when you have the crash handler output and a core
   that opens in gdb with `bt` matching the handler's backtrace.
2. **Easy - write the update.** For a fictional incident ("balance enquiry API returning errors
   for 20% of requests since 10:05"), write update 1 (minute 2) and update 2 (minute 15, after a
   restart of one node fixed it) using the template in 2.5. Done when each update has status,
   impact, known facts, actions, a request to the client and the next update time, and no
   hypothesis is stated as fact.
3. **Medium - collect evidence from a hang.** Run `m10_deadlock_debug` and run
   `m11_collect_evidence.sh --pid <PID>` against it. Open both cores from the tarball. Done when you
   can show the two cores have identical stacks for the deadlocked threads (proving deadlock, not
   slowness) and the tarball contains binary, libs and logs.
4. **Medium - structured logging.** Add the `log_event` function from 3.3 and a `thread_local`
   request id to `m10_data_race.cpp` (each thread logs `req.start` and `req.end` around its loop).
   Done when you can grep the output by `tid=` and by `req=` and the tid matches `info threads`
   LWP in a core of the program.
5. **Hard - find the killer request.** Extend exercise 4: give each of 1000 simulated requests an
   id, make request number 637 trigger a null dereference, install the crash handler. Using only
   the log file and the core (not the source), identify the request id that crashed the process
   and the thread that ran it. Done when the awk one-liner in 2.7 (adapted) prints that request
   id and it matches the crash handler's `request =` line.
6. **Hard - runbook.** Write a one-page runbook for a service you know from work (anonymised):
   severity rules, dump capture on Windows and Linux, failover, feature flags, who to call,
   update template. Done when someone who has never seen the service could take a correct dump
   and send a correct first update from the page alone.

## 7. Interview questions

**Q: A production server is hung. The client wants it restarted now. What do you do?**
A: Agree to the restart and ask for 60-120 seconds first: take two dumps (`gcore` or
`procdump -ma`) about 30-60 seconds apart, grab logs and per-thread CPU, then restart or fail over.
The dumps prove whether it is a deadlock (identical stacks) or slowness, and without them the root
cause is guesswork and the hang will recur. If a standby node exists, fail over first and keep the
hung node isolated for analysis.

**Q: How do you decide incident severity?**
A: By business impact, not technical complexity: which function is affected, how many users or
transactions, whether money or data integrity is at risk, and whether a workaround exists. Wrong
results outrank unavailability. Severity is re-evaluated at every update and repeated
"self-healing" crashes are escalated because they will eventually hit a critical window.

**Q: What should every log line in a multithreaded server contain?**
A: A precise timestamp with timezone, level, pid, kernel thread id, request or correlation id,
component, a stable event name and key fields in structured form, with sensitive data masked.
The thread id joins the log to a thread in the core; the request id joins events across threads
and services; the event name makes logs countable.

**Q: Why must a crash handler only call async-signal-safe functions?**
A: The handler interrupts the faulting thread at an arbitrary point, possibly inside `malloc` or
`printf` with their internal locks held or their data structures half-updated. Calling them again
deadlocks or corrupts further, and a deadlocked crash handler means no core at all. So the handler
uses `write`, preformatted buffers and `backtrace_symbols_fd`, runs on an alternate stack, and
re-raises the signal with the default action so the core is still written.

**Q: How do you correlate a core dump with application logs?**
A: The crash handler (or the last log lines) gives the kernel tid and request id; `info threads`
in gdb shows the LWP of each thread, so the faulting thread's LWP finds its log lines; the
request-start-without-request-end line for that tid is the request that was running. Timestamps
from the core file, `journalctl` or the Windows event log 1000 entry align the rest, assuming NTP
is working.

**Q: What makes a restart "safe" in a payment system?**
A: Every money-moving operation is idempotent through a unique transaction reference, the database
transaction is the source of truth so partial in-memory state is discarded harmlessly, and the
restart drains in-flight work or relies on the DB to roll back incomplete transactions. A
supervisor restarts on failure with a burst limit so a crash loop stops and alerts.

**Q: What is a blameless postmortem and why does it matter?**
A: A review that asks how the system allowed the failure rather than who caused it. People share
the uncomfortable facts (the skipped review, the ignored warning) only when they are not punished
for them, and those facts are exactly where the prevention lies. Its outputs are an evidence-based
timeline and owned, dated, verifiable action items that are tracked to closure.

## 8. Recap

- The first 15 minutes: acknowledge, scope, classify, capture, mitigate, verify - in that order.
- Severity is business impact; data integrity outranks availability; re-evaluate every update.
- Capture before restart: two dumps for a hang, the core plus binary plus symbols plus libs for a
  crash, logs and "what changed" for everything. Script it.
- Dumps contain customer data: secure channel, encrypted, access-controlled, deleted after.
- Mitigate with the smallest blast radius: block input, flag off, fail over, restart, roll back,
  stop.
- Client communication: one voice, fixed cadence, facts separate from hypotheses, never promise a
  root-cause time.
- Log by design: timestamp with zone, pid, kernel tid, request id, stable event name, structured
  fields, masked data; request start and end; startup build info; resource gauges.
- Crash handler: async-signal-safe only, alternate stack, warm `backtrace`, print request id and
  tid, re-raise so the core survives.
- Safe restarts need idempotency, DB-owned truth, graceful drain and a restart burst limit.
- Postmortems are blameless, evidence-based and produce owned, dated, verifiable actions.
