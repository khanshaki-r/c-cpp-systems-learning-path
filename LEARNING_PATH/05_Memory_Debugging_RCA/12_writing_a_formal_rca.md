# 12. Writing a formal RCA

**Time:** ~3 hours   **Prerequisites:** 06, 07, 10, 11

## 1. In plain words

A root cause analysis (RCA) is the document that explains, after an incident, what happened, why
it happened, and what you have done so it cannot happen again. For a bank, it is also a contract
artefact: the support agreement usually requires one within a fixed time, the bank's IT risk team
reviews it, and parts of it may go into their report to the regulator.

Think of an aircraft accident report. It does not say "the plane crashed because it hit the
ground". It says the pilot descended because the altimeter read wrong, because a sensor was iced,
because the heater had failed, because the maintenance check did not include the heater. Each
step has evidence. The fix is not "tell pilots to be careful"; it is "add the heater to the
checklist and add an alarm". A good software RCA has the same shape: a chain of evidence from the
symptom to the defect, from the defect to the process gap that let it ship, and fixes for both.

The hard part is not the writing. It is being honest about what you proved and what you only
believe, separating the thing that happened that night (the trigger) from the thing that was
wrong all along (the root cause), and writing it so a bank's operations head and its chief
architect both come away trusting your team more than before the incident.

## 2. The real thing

### 2.1 The structure

Every RCA in this course uses this structure. Most banks and vendors use a variant of it; if the
client has a template, use theirs and map these sections onto it.

| # | Section | What goes in | Length |
|---|---------|--------------|--------|
| 0 | Header | Incident id, title, severity, service, environment, dates, author, reviewers, version, status (preliminary/final) | table |
| 1 | Executive summary | What happened, impact, root cause, fix, in plain words. A reader who stops here knows everything important. | 4-8 sentences |
| 2 | Impact | Who and what was affected, duration, counts, financial and data impact, SLA impact | table + few lines |
| 3 | Timeline | Timestamped events from first symptom to resolution, with sources | table |
| 4 | Root cause | The defect, the mechanism from defect to failure, the evidence | 1-2 pages |
| 5 | Trigger | Why it happened *that day* | a paragraph |
| 6 | Contributing factors | Conditions that made it possible, worse or slower to fix | bullets |
| 7 | 5-whys | The causal chain from symptom to process gap | numbered list |
| 8 | Resolution / fix | Immediate mitigation and permanent fix, with versions | bullets |
| 9 | Preventive actions | Owned, dated actions for the process gaps | table |
| 10 | Verification | How you proved the fix works and how you will know it holds | bullets |
| 11 | Appendix | Evidence excerpts: stacks, log lines, tool output, references | as needed |

### 2.2 Trigger versus root cause

This is the distinction that separates a senior RCA from a junior one.

- **Trigger**: the event or condition that made the latent problem visible at that moment. A
  traffic spike, a particular input, a config change, a month-end batch, an OS patch.
- **Root cause**: the defect or design flaw that made the system fail when triggered. Remove the
  root cause and the trigger becomes harmless. Remove only the trigger and the system fails again
  on the next one.

| Wrong (trigger presented as root cause) | Right |
|------------------------------------------|-------|
| "Root cause: unusually high load at month-end." | Trigger: month-end load. Root cause: a race in session expiry that needs two concurrent requests on the same session; it only becomes likely at high load. |
| "Root cause: a malformed message from the partner bank." | Trigger: a 13-character account id. Root cause: the parser writes one byte past a 12-byte buffer and does not validate field lengths. |
| "Root cause: the server ran out of memory." | That is a symptom. Root cause: a cache with no eviction bound. |
| "Root cause: human error during deployment." | Root cause: a deployment step that can be done wrong without any check failing. The fix is the check. |

A useful test: **"If this condition happens again tomorrow, will the system now survive?"** If the
answer depends on the condition not happening, you have described the trigger.

There can be more than one root cause (a defect plus a missing safeguard). And there is often a
second level: the *technical* root cause (the bug) and the *process* root cause (why review, tests
and tools did not catch it). A good RCA names both and fixes both.

### 2.3 Evidence

Every claim in the root-cause section is one of:

- **Proven**: you have direct evidence - a stack from the dump, a log line, a reproduction that
  fails before the fix and passes after, a tool report (ASan, TSan, `!heap -p -a`).
- **Inferred**: it follows from proven facts by reasoning you can state.
- **Hypothesis**: consistent with the evidence but not proven.

Label them. "The dump shows" (proven), "therefore" (inferred), "we believe" (hypothesis). An RCA
whose root cause is a hypothesis is a *preliminary* RCA; say so, say what evidence would confirm
it, and say what detection you added to get that evidence next time (module 10's ring buffer,
asserts, PageHeap on one node). Never present a hypothesis as proven to close an incident faster:
if it is wrong, the incident recurs and the next RCA starts with zero credibility.

Put raw evidence in the appendix (stacks, log excerpts, tool reports, trimmed to the relevant
lines) and reference it from the body: "(Appendix A.2)".

### 2.4 5-whys

The 5-whys is a way to walk from the symptom to the process gap by asking "why?" repeatedly. Five
is a guideline, not a rule; stop when the next answer is outside your control or is a process you
can change.

Rules that make it work:

- Each answer must be supported by evidence or be clearly a process statement.
- Follow the most important branch; note other branches as contributing factors.
- Do not stop at the bug. "Why did the bug exist?" leads to "why did review not catch it?" and
  "why did tests not catch it?". That is where prevention lives.
- Never end at a person. "Because the developer made a mistake" is always followed by "why did the
  system let a mistake reach production?".

### 2.5 Tone for a banking client

- **Factual, calm, precise.** No adjectives that judge ("unfortunately", "silly", "minor glitch").
  Numbers with units and timestamps with timezone.
- **Own it.** "A defect in our session handling code" - not "an unexpected condition", not "a
  third-party library" unless you proved it.
- **No blame, no names.** Internal names never appear. Teams can appear as roles ("vendor
  support", "bank operations").
- **No excessive apology, no defensiveness.** One sentence acknowledging impact is right; three
  paragraphs read as evasive.
- **Explain technical terms once, in plain words.** The operations head must follow the summary;
  the architect must find the mechanism precise. Put the plain version in the summary and the
  precise version in section 4.
- **Commit only to what you control.** Dates for your actions; "recommend" for the bank's.
- **Be explicit about data.** Banks care first whether any transaction was lost, duplicated or
  posted wrongly. State it, and state how you verified it (reconciliation query, count).
- **Same terms everywhere.** If the summary says "fund transfer service", the timeline does not
  switch to "FT module" and "xferd".

### 2.6 Preventive actions that actually prevent

A preventive action is good when it is **specific, owned, dated and verifiable**:

| Weak | Strong |
|------|--------|
| Improve code review. | Add "every buffer write is bounded by the destination size" to the review checklist for parser code; owner: dev lead; by 2026-11-15; done when the checklist is in the merge-request template. |
| Add more testing. | Add a libFuzzer target for the ISO 8583 parser running 30 minutes nightly; owner: Shakir; by 2026-11-30; done when the job runs and its first week's results are reviewed. |
| Monitor memory. | Alert when RSS of the service grows more than 200 MB in 6 hours; owner: support lead; by 2026-10-25. |

Classify actions by what they do: **eliminate** the bug class (RAII, bounded types), **detect**
it earlier (sanitizers in CI, fuzzing, static analysis), **contain** it (input validation, flags,
restart limits), **diagnose** faster (logging, dump config). Module 13 is the catalogue.

### 2.7 Verification

"We fixed it" needs proof, in two parts:

- **The fix fixes it.** A reproduction (test, fuzz input, stress run) that fails on the old build
  and passes on the new one. Sanitizer runs clean. For races: TSan clean, stress test passes N
  iterations that previously failed within M.
- **The fix holds in production.** A metric or log signal you will watch, for how long, and what
  value means success: "zero occurrences of event `sess.uaf_guard` and zero crashes over 30 days
  including month-end".

## 3. Code

There is no program for this module. The "code" is the two complete RCAs below. They are
**illustrative**: the bank, the system, the people, the numbers and the timestamps are invented
for teaching, though the bug mechanisms are real and match the programs in `code/`. Use them as
templates. Copy the structure, not the facts.

### 3.1 Example RCA 1: heap memory corruption

---

> **ILLUSTRATIVE EXAMPLE - fictional bank, system and data, written as a template.**

**Root Cause Analysis - INC-2026-0917: Payment Gateway Adapter process terminations**

| Field | Value |
|-------|-------|
| Incident id | INC-2026-0917 |
| Client | Example Bank Ltd. (fictional) |
| Service | Payment Gateway Adapter (PGA), version 4.12.3 |
| Environment | Production, data centre DC1, nodes PGA-1 and PGA-2 (Linux, RHEL 8) |
| Severity | Sev-2 (degraded; partial outage with automatic retry) |
| Incident window | 2026-09-17 11:02 IST to 2026-09-17 14:40 IST (mitigated); permanent fix 2026-09-21 |
| Document status | Final, version 1.1 (preliminary v1.0 issued 2026-09-18) |
| Prepared by | Vendor product support, core engineering |
| Reviewed by | Vendor engineering head; bank IT application owner |

**1. Executive summary**

On 17 September 2026, between 11:02 and 14:40 IST, the Payment Gateway Adapter process terminated
unexpectedly 14 times across two nodes. Each termination interrupted in-flight requests for a few
seconds until the process restarted automatically. 312 payment requests failed and were retried
successfully by the upstream channel; no transaction was lost, duplicated or posted with a wrong
amount (verified by reconciliation, section 2).

The root cause is a defect in the PGA's message parser: when a beneficiary reference field is
longer than 35 characters, the parser writes past the end of a memory buffer, corrupting the
memory allocator's internal data. The process then terminates later, when the allocator detects
the corruption. The defect has existed since version 4.10.0 (released March 2026); it was
triggered on 17 September when a partner bank began sending a longer reference format.

Service was restored at 14:40 IST by rejecting over-length references at the input boundary. The
permanent fix (version 4.12.4) bounds the copy and validates every field length; it was deployed on
21 September. We are adding fuzz testing and memory-error detection to our build pipeline to find
this class of defect before release.

**2. Impact**

| Item | Value | Source |
|------|-------|--------|
| Duration of impact | 3 h 38 min (11:02 - 14:40 IST) | PGA logs, monitoring |
| Process terminations | 14 (PGA-1: 8, PGA-2: 6) | systemd journal, core files |
| Failed requests | 312 payment requests returned a connection error to the channel | PGA access log, channel logs |
| Requests retried successfully | 312 of 312, by the channel's automatic retry | Channel logs, transaction reference match |
| Lost / duplicated / mis-posted transactions | None | Reconciliation query on 17 Sep, 18:00 IST: all 312 references posted exactly once, amounts match |
| Customer-visible effect | Payment confirmation delayed by up to 40 s for affected customers | Channel latency report |
| Data exposure | None | - |
| SLA | Availability for the day 98.7% against 99.5% target | Monitoring |

**3. Timeline (all times IST, 2026-09-17 unless stated)**

| Time | Event | Source |
|------|-------|--------|
| 2026-03-12 | Version 4.10.0 released, containing the defective parser change | Release notes |
| 10:30 | Partner bank switches to extended beneficiary reference format (up to 40 characters) | Partner bank change notice, received after the incident |
| 11:02 | First PGA termination on PGA-1, `SIGABRT`, core written | journal, core file |
| 11:02 - 12:10 | 6 further terminations, auto-restarted by systemd | journal |
| 12:14 | Monitoring alert: PGA restart count > 5 per hour | Alerting system |
| 12:20 | Vendor support acknowledges, Sev-3 opened | Ticket |
| 12:45 | Severity raised to Sev-2 (terminations continuing on both nodes) | Ticket |
| 12:50 | Bank operations and vendor bridge call opened; updates every hour agreed | Bridge notes |
| 13:05 | Three core files and binaries collected with evidence script | Evidence log, SHA-256 recorded |
| 13:40 | Core analysis: all cores abort inside the memory allocator with heap-corruption messages; the faulting stacks differ | Analysis notes (Appendix A.1) |
| 14:05 | Log correlation: every termination preceded within 2 s by a request with a reference longer than 35 characters | PGA logs (Appendix A.2) |
| 14:20 | Reproduced in test with a 40-character reference under AddressSanitizer: heap-buffer-overflow in `parse_beneficiary` | Test log (Appendix A.3) |
| 14:35 | Mitigation deployed: input rule rejects references > 35 characters with a business error; partner bank informed | Change record CHG-5521 |
| 14:40 | Last termination before mitigation took effect; none since | journal |
| 15:30 | Service declared stable; Sev-2 downgraded to Sev-3 pending permanent fix | Ticket |
| 18:00 | Reconciliation confirms no lost, duplicated or mis-posted transactions | Reconciliation report |
| 09-18 17:00 | Preliminary RCA (v1.0) issued | - |
| 09-21 01:00 | Version 4.12.4 deployed in the maintenance window; input rule removed after verification | Change record CHG-5530 |
| 10-21 | 30-day observation ends with zero terminations; incident closed | Monitoring |

**4. Root cause**

*4.1 The defect.* `parse_beneficiary()` copies the beneficiary reference into a 36-byte field of
the `PaymentMessage` structure (35 characters plus terminator). The copy loop is bounded by the
length of the *input* field, not the destination:

```cpp
// version 4.10.0 - 4.12.3
for (size_t i = 0; i < field.len; ++i)      // field.len comes from the wire
    msg->beneficiary_ref[i] = field.data[i];
msg->beneficiary_ref[field.len] = '\0';
```

For a 40-character reference this writes 4 characters and a terminator (5 bytes) past the end of
`beneficiary_ref`. `PaymentMessage` is allocated on the heap and `beneficiary_ref` is its last
member, so the excess bytes overwrite the allocator's header of the next heap block.

*4.2 Why the process terminated later and in different places.* The glibc allocator stores the
size of each block in a header just before it. The overwritten header is not checked when it is
written; it is checked when that neighbouring block is freed or merged. That happens in whatever
code next frees memory near the corrupted block - often in a different thread, milliseconds to
seconds later. The allocator then detects an impossible size and stops the process deliberately
(`malloc(): invalid next size`, `free(): invalid next size (fast)`). This is why the 14 core files
show 5 different stacks, none of them in the parser (Appendix A.1). Heap corruption typically
presents this way: the crash site shows where the damage was *found*, not where it was *done*.

*4.3 Evidence chain.*

1. **Proven** - all 14 terminations are `SIGABRT` raised by glibc's heap consistency checks
   (core files, `p $_siginfo`, abort messages in the journal). Appendix A.1.
2. **Proven** - in all 14 cases, the same thread (matching kernel thread id) processed a request
   with a beneficiary reference of 36-40 characters between 0.1 and 1.8 seconds before the
   termination; no termination occurred without such a request. Appendix A.2.
3. **Proven** - in 3 analysed cores, the heap block adjacent to a `PaymentMessage` contains the
   ASCII tail of a beneficiary reference where the allocator header should be
   (`x/8xg` on the block). Appendix A.1.
4. **Proven** - the test build with AddressSanitizer reports `heap-buffer-overflow ... WRITE of
   size 1 ... in parse_beneficiary payment_parser.cpp:212` for a 40-character reference, located
   0 bytes after the 168-byte `PaymentMessage` region. Appendix A.3.
5. **Proven** - with version 4.12.4, the same test and 10 million fuzzed messages produce no
   sanitizer reports (section 10).

**5. Trigger**

At 10:30 IST on 17 September a partner bank began sending beneficiary references of up to 40
characters, permitted by its updated message specification. References longer than 35 characters
had not appeared in production before that day (checked against 90 days of logs). The trigger is
the new input; the root cause is the parser's failure to bound its copy and validate field length.

**6. Contributing factors**

- No input validation at the gateway boundary for field lengths; the parser trusted the wire.
- The parser change in 4.10.0 was reviewed, but the review checklist had no item for bounded
  copies, and unit tests used only valid-length references.
- Memory-error detection (AddressSanitizer) and fuzz testing were not part of the release pipeline.
- The restart alert threshold (5 per hour) delayed engagement by 72 minutes after the first
  termination.
- The partner bank's format change was not communicated to the vendor in advance.

**7. 5-whys**

1. Why did requests fail? The PGA process terminated 14 times.
2. Why did it terminate? The memory allocator detected corrupted heap metadata and aborted.
3. Why was the heap metadata corrupted? `parse_beneficiary` wrote up to 5 bytes past a 36-byte
   field for references longer than 35 characters.
4. Why did the parser write past the field? The copy was bounded by the input length, not the
   destination size, and field lengths were not validated against the specification.
5. Why did this reach production? Tests covered only valid-length inputs, review had no
   bounded-copy check, and the pipeline had no fuzzing or AddressSanitizer stage that would have
   found it in seconds.

**8. Resolution**

- *Mitigation (17 Sep, 14:35)*: input rule at the gateway rejects beneficiary references longer
  than 35 characters with business error `PGA-422`, so the partner bank's system receives a clear
  rejection instead of a dropped connection. Partner bank informed at 14:50.
- *Permanent fix (version 4.12.4, 21 Sep)*: the parser validates every field against the maximum
  length in the specification and rejects the message if exceeded; copies use a bounded helper
  that cannot exceed the destination size; `beneficiary_ref` changed to a fixed-capacity string
  type with checked assignment. All 23 other fields of the parser reviewed for the same pattern;
  2 further unbounded copies found and fixed (not yet triggered).
- *Extended format*: support for 40-character references is scheduled for version 4.13 as a
  planned change, with the partner bank's specification.

**9. Preventive actions**

| # | Action | Type | Owner | Due | Status |
|---|--------|------|-------|-----|--------|
| P1 | Add libFuzzer targets for all wire-format parsers; run 30 min per target nightly; crashes block release | Detect | Vendor core engineering | 2026-10-31 | In progress |
| P2 | Add AddressSanitizer + UndefinedBehaviorSanitizer build and full regression run to CI for every merge | Detect | Vendor build team | 2026-10-15 | Done |
| P3 | Add "every copy bounded by destination size; every wire field length validated" to the parser review checklist | Eliminate | Vendor dev lead | 2026-09-30 | Done |
| P4 | Replace raw `char[]` fields in message structures with a bounded string type across the PGA | Eliminate | Vendor core engineering | 2026-12-15 | Planned |
| P5 | Lower restart alert threshold to 2 per 10 minutes and page on-call | Diagnose | Bank operations (recommended) | 2026-10-05 | Done |
| P6 | Agree notice period for partner message format changes | Contain | Bank integration team (recommended) | 2026-10-31 | Open |

**10. Verification**

- The reproduction test (40-character reference) fails under AddressSanitizer on 4.12.3 and passes
  on 4.12.4. Added to the regression suite as `test_beneficiary_ref_overlength`.
- 10 million fuzzer-generated messages on 4.12.4 with AddressSanitizer: no reports.
- Production: zero terminations from 21 Sep to 21 Oct, including month-end. Watched via restart
  count and the new `pga.field_rejected` counter (14 over-length references rejected cleanly during
  the period, all from the same partner, before their 4.13 switch).

**Appendix A - Evidence (excerpts)**

A.1 Core analysis, core 1 of 14 (PGA-1, 11:02:41):

```
Program terminated with signal SIGABRT, Aborted.
(gdb) p $_siginfo.si_signo
$1 = 6
(gdb) bt 8
#0  __pthread_kill_implementation (...) at pthread_kill.c:44
#1  raise (sig=6) at ../sysdeps/posix/raise.c:26
#2  abort () at abort.c:79
#3  __libc_message (fmt="%s\n") at ../sysdeps/posix/libc_fatal.c:150
#4  malloc_printerr (str="free(): invalid next size (fast)") at malloc.c:5664
#5  _int_free (...) at malloc.c:4472
#6  AuditRecord::~AuditRecord () at audit.cpp:58
#7  AuditQueue::flush () at audit.cpp:131
(gdb) x/6xg 0x7f2c4c012a40          # chunk following PaymentMessage at 0x7f2c4c0129a0
0x7f2c4c012a40: 0x0000000000003334  0x0000000000000000   <- "43" ASCII where size field expected
```

A.2 Log correlation (PGA-1; same kernel thread id 48213 in log and core):

```
ts=2026-09-17T11:02:40.912+05:30 lvl=INFO tid=48213 req=a91f... evt=req.start src=PARTNER-B ref_len=40
ts=2026-09-17T11:02:41.004+05:30 lvl=INFO tid=48213 req=a91f... evt=req.end result=OK
(process terminated 11:02:41.337, journal: code=dumped status=6/ABRT)
```

A.3 AddressSanitizer reproduction (test build of 4.12.3):

```
==9124==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x60e000000128
WRITE of size 1 at 0x60e000000128 thread T0
    #0 parse_beneficiary(...) payment_parser.cpp:212
    #1 parse_payment(...) payment_parser.cpp:388
0x60e000000128 is located 0 bytes after 168-byte region [0x60e000000080,0x60e000000128)
allocated by thread T0 here:
    #0 operator new(unsigned long)
    #1 PaymentMessage::create() payment_message.cpp:41
```

---

### 3.2 Example RCA 2: deadlock

---

> **ILLUSTRATIVE EXAMPLE - fictional bank, system and data, written as a template.**

**Root Cause Analysis - INC-2026-1003: Core Banking Application Server unresponsive**

| Field | Value |
|-------|-------|
| Incident id | INC-2026-1003 |
| Client | Example Bank Ltd. (fictional) |
| Service | Core Banking Application Server (CBAS), version 7.3.1, RPC-over-TCP |
| Environment | Production, node CBAS-APP-3 (Windows Server 2022) |
| Severity | Sev-1 (all branch transactions on one cluster blocked) |
| Incident window | 2026-10-03 18:47 IST to 2026-10-03 19:21 IST |
| Document status | Final, version 1.0 |
| Prepared by | Vendor product support, core engineering |
| Reviewed by | Vendor engineering head; bank IT head of applications |

**1. Executive summary**

On 3 October 2026 between 18:47 and 19:21 IST, the core banking application server on node
CBAS-APP-3 stopped responding. Branch users connected to that node (61 branches in the western
region) could not complete transactions for 34 minutes; other nodes were unaffected. The server
was restarted at 19:19 after a memory snapshot was taken, and service resumed at 19:21. No
transaction was lost, duplicated or posted wrongly; transactions in progress at 18:47 were rolled
back by the database and were re-entered by the branches.

The root cause is a deadlock: two internal locks in the server could be taken in opposite orders
by two different operations - the end-of-day cash position update and the session timeout sweeper.
When both ran at the same moment on the same branch session, each waited for the other forever,
and all later requests queued behind them. The defect was introduced in version 7.3.0 (August
2026). It was triggered on 3 October by an extended end-of-day window that overlapped the sweeper's
schedule. The fix (version 7.3.2) enforces a single lock order; it was deployed on 6 October.

**2. Impact**

| Item | Value | Source |
|------|-------|--------|
| Duration | 34 min (18:47 - 19:21 IST) | Server logs, monitoring |
| Affected | 61 branches (western region cluster), approx. 420 branch users | Session table in dump, connection logs |
| Requests not processed | 1,874 requests timed out at the branch client | Branch client logs aggregated by bank operations |
| Transactions in progress at 18:47 | 37, rolled back by the database (not committed) | DB logs |
| Lost / duplicated / mis-posted transactions | None | Reconciliation 3 Oct 21:00 IST: 37 rolled-back references re-entered once each; no orphan postings |
| Internet / mobile banking | Not affected (served by other nodes) | Monitoring |
| SLA | Sev-1 response met (acknowledged in 6 min); availability breach for the western cluster for the day | Contract report |

**3. Timeline (all times IST, 2026-10-03 unless stated)**

| Time | Event | Source |
|------|-------|--------|
| 2026-08-14 | Version 7.3.0 released; adds locking of the session object during cash position update | Release notes |
| 18:30 | End-of-day cash position job started, 30 min later than usual (bank-requested extension) | Job scheduler log |
| 18:47:12 | Session sweeper starts its 15-minute cycle | Server log `evt=sweep.start` |
| 18:47:12 | Last completed request on CBAS-APP-3; request queue starts growing | Server log, queue-depth metric |
| 18:49 | Branch helpdesk receives first "transaction hanging" calls | Bank helpdesk |
| 18:52 | Monitoring alert: CBAS-APP-3 queue depth > 500, zero completions | Alerting |
| 18:58 | Vendor on-call acknowledges, Sev-1 declared; bridge opened; updates every 30 min | Ticket |
| 19:04 | CPU near 0% on the process, threads alive: hang, not overload | Performance monitor |
| 19:08 | Two full memory dumps taken 60 s apart with `procdump -ma` | Evidence log, SHA-256 recorded |
| 19:12 | First dump analysed: 2 worker threads waiting on each other's lock; 46 workers waiting behind them | Analysis (Appendix B.1) |
| 19:15 | Second dump: identical stacks for all blocked threads - deadlock confirmed | Analysis |
| 19:19 | Process restarted; branch connections redistributed | Change record EMG-2210 |
| 19:21 | Transactions completing normally; queue depth zero | Monitoring |
| 19:30 | Update to bank: service restored, cause identified as internal deadlock, fix in progress | Bridge notes |
| 21:00 | Reconciliation complete: no lost, duplicated or mis-posted transactions | Reconciliation report |
| 10-04 16:00 | Reproduced in test with stress harness; ThreadSanitizer reports lock-order inversion on 7.3.1 | Test log (Appendix B.3) |
| 10-06 02:00 | Version 7.3.2 deployed to all nodes | Change record CHG-5588 |
| 11-05 | 30-day observation (including month-end) complete, no recurrence; incident closed | Monitoring |

**4. Root cause**

*4.1 The defect.* The server protects two pieces of shared state with two locks:

- `cash_lock` - protects the branch cash position table.
- `session_lock` (per branch session) - protects the session object.

Version 7.3.0 changed the cash position update so that, while holding `cash_lock`, it also takes
the branch's `session_lock` to stamp the session with the new position. The session sweeper, which
already existed, takes `session_lock` and then, for sessions with open cash drawers, takes
`cash_lock` to release the drawer:

```
Cash position update (thread A)        Session sweeper (thread B)
  lock(cash_lock)                        lock(session_lock[branch 0417])
  ...                                    ...
  lock(session_lock[branch 0417])  <-->  lock(cash_lock)
  waits forever for B                    waits forever for A
```

If both run for the same branch session at the same moment, each holds the lock the other needs.
Neither can proceed and neither ever releases. This is a lock-order inversion deadlock. All
further requests that need `cash_lock` (every cash transaction) or that branch's session queue
behind them, and within about a minute every worker thread is blocked.

*4.2 Evidence chain.*

1. **Proven** - process CPU near 0% with all threads alive, and two dumps 60 s apart show identical
   stacks for every blocked thread: nothing was progressing (Appendix B.1, B.2). This rules out an
   overload or slow database.
2. **Proven** - `!locks` shows `cash_lock` (a critical section) owned by thread 0x1a2c, which is
   waiting in `EnterCriticalSection` for `session_lock` of branch session 0417, owned by thread
   0x2f40. Thread 0x2f40 is waiting for `cash_lock`. That is a cycle (Appendix B.1).
3. **Proven** - thread 0x1a2c's stack is in `CashPosition::apply`; thread 0x2f40's stack is in
   `SessionSweeper::expire`. Their call paths take the locks in opposite orders (source review of
   7.3.1, cited lines in Appendix B.1).
4. **Proven** - 46 other worker threads are waiting on `cash_lock` (`!locks` contention list),
   explaining the total loss of service.
5. **Proven** - in test, a stress harness running both operations against one session deadlocks
   within 3 minutes on 7.3.1; ThreadSanitizer (Linux test build) reports
   `lock-order-inversion (potential deadlock)` naming the same two functions, even on runs that did
   not hang. On 7.3.2: no deadlock in 8 hours and no TSan report (Appendix B.3, section 10).

**5. Trigger**

The sweeper and the cash position update had never run at the same time in production: the update
normally finishes by 18:30 and the sweeper's cycles were at 18:32 and 18:47. On 3 October the bank
extended the end-of-day window by 30 minutes, so the cash position update ran from 18:30 and
overlapped the 18:47 sweep. The overlap made the deadlock possible; it occurred when both reached
branch session 0417 at the same moment. The extension is the trigger; the opposite lock order is
the root cause. Without a fix, any overlap - for example on a slow month-end - could cause it again.

**6. Contributing factors**

- No documented global lock order existed, so the 7.3.0 change could not be checked against one.
- Concurrency tests ran the cash update and the sweeper separately, never together.
- ThreadSanitizer was not part of CI (the Windows production build has no TSan; the Linux test
  build supports it but was not run under it).
- The server had no lock-wait watchdog; a hang was detected by queue-depth alert after 5 minutes
  and diagnosed only by dump.
- Restart procedure was correct (dumps taken first), which made the 30-minute diagnosis possible.

**7. 5-whys**

1. Why did branches lose service? All worker threads on CBAS-APP-3 were blocked.
2. Why were they blocked? They waited for `cash_lock`, held by a thread that was itself blocked.
3. Why was that thread blocked forever? It waited for a session lock held by the sweeper, which
   waited for `cash_lock`: a lock-order cycle.
4. Why could the locks be taken in opposite orders? The 7.3.0 change added a nested lock without
   a defined global lock order to check against.
5. Why was it not caught before release? No documented lock order, no test running both
   operations concurrently, and no ThreadSanitizer stage, which would have reported the inversion
   from a single non-hanging run.

**8. Resolution**

- *Mitigation (3 Oct)*: dumps captured, process restarted at 19:19. Until the fix, the sweeper
  schedule was moved to avoid the end-of-day window (configuration change, 3 Oct 22:00).
- *Permanent fix (version 7.3.2, 6 Oct)*: the cash position update no longer takes `session_lock`
  while holding `cash_lock`; it records the session stamp after releasing `cash_lock`. A global
  lock order (session before cash) is documented and enforced in debug and test builds by a lock-
  rank checker that fails immediately on an out-of-order acquisition. The sweeper's schedule
  restored.
- All 14 other code paths that take two or more locks reviewed against the new order; one more
  latent inversion found (report generation versus branch close) and fixed in 7.3.2.

**9. Preventive actions**

| # | Action | Type | Owner | Due | Status |
|---|--------|------|-------|-----|--------|
| P1 | Document the global lock order in the architecture guide; review checklist item "new nested lock respects the order" | Eliminate | Vendor architect | 2026-10-20 | Done |
| P2 | Lock-rank checker enabled in all debug and test builds | Detect | Vendor core engineering | 2026-10-06 | Done (in 7.3.2) |
| P3 | ThreadSanitizer stage in CI on the Linux test build for the concurrency test suite | Detect | Vendor build team | 2026-10-31 | In progress |
| P4 | Concurrency test that runs all scheduled jobs against live sessions simultaneously | Detect | Vendor QA | 2026-11-15 | Planned |
| P5 | Lock-wait watchdog: log thread ids and lock names when a lock wait exceeds 10 s; alert | Diagnose | Vendor core engineering | 2026-11-30 | Planned |
| P6 | Alert on "zero completed requests for 60 s while queue > 0" | Diagnose | Bank operations (recommended) | 2026-10-15 | Done |

**10. Verification**

- Stress harness: 7.3.1 deadlocks within 3 minutes (5 of 5 runs); 7.3.2 runs 8 hours without
  deadlock (3 runs).
- ThreadSanitizer: lock-order-inversion report on 7.3.1; clean on 7.3.2 for the full concurrency
  suite.
- Lock-rank checker: deliberately reverting the fix in a test build makes the checker abort at the
  first out-of-order acquisition with both lock names, on every run.
- Production: no hang and zero lock-watchdog events from 6 Oct to 5 Nov, including month-end
  (31 Oct) with the original sweeper schedule.

**Appendix B - Evidence (excerpts)**

B.1 Dump 1 (19:08:22), WinDbg:

```
0:000> !locks
CritSec cbas!g_cash_lock+0 at 00007ff6`4a2c1e40
WaiterWoken        No
LockCount          47
RecursionCount     1
OwningThread       1a2c
EntryCount         0
ContentionCount    2f
*** Locked

CritSec +2d81f3a8 at 000001d3`2d81f3a8          (Session 0417 session_lock)
LockCount          1
RecursionCount     1
OwningThread       2f40
*** Locked

0:000> ~~[1a2c]s ; k 6
 # Child-SP          RetAddr           Call Site
00 0000003c`9a6ff1a8 00007ffc`1e2d5c6e ntdll!NtWaitForAlertByThreadId+0x14
01 0000003c`9a6ff1b0 00007ffc`1e2a1a3b ntdll!RtlpWaitOnCriticalSection+0x1ee
02 0000003c`9a6ff2b0 00007ffc`1e2a1860 ntdll!RtlpEnterCriticalSectionContended+0x1cb
03 0000003c`9a6ff310 00007ff6`4a1b33c2 ntdll!RtlEnterCriticalSection+0x40
04 0000003c`9a6ff340 00007ff6`4a1b2a17 cbas!Session::lock+0x12 [session.cpp @ 88]
05 0000003c`9a6ff370 00007ff6`4a19e450 cbas!CashPosition::apply+0x1a7 [cash_position.cpp @ 214]

0:000> ~~[2f40]s ; k 6
 ...
03 0000003c`9b0ff0f0 00007ff6`4a19e9c1 ntdll!RtlEnterCriticalSection+0x40
04 0000003c`9b0ff120 00007ff6`4a1c0f22 cbas!CashDrawer::release+0x31 [cash_drawer.cpp @ 57]
05 0000003c`9b0ff160 00007ff6`4a1c0b10 cbas!SessionSweeper::expire+0xd2 [sweeper.cpp @ 143]
```

Holds / waits-for:

| Thread | Holds | Waits for | Held by |
|--------|-------|-----------|---------|
| 0x1a2c (CashPosition::apply) | `g_cash_lock` | session_lock 0417 | 0x2f40 |
| 0x2f40 (SessionSweeper::expire) | session_lock 0417 | `g_cash_lock` | 0x1a2c |
| 46 workers | - | `g_cash_lock` | 0x1a2c |

B.2 Dump 2 (19:09:23): `~*k` output for threads 0x1a2c, 0x2f40 and the 46 workers identical to
dump 1 frame for frame; `!locks` `ContentionCount` unchanged.

B.3 ThreadSanitizer, Linux test build of 7.3.1, stress harness:

```
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=30117)
  Cycle in lock order graph: M1 (0x7b0c00000040) => M2 (0x7b1000001a80) => M1
  Mutex M2 acquired here while holding mutex M1 in thread T3:
    #1 Session::lock() session.cpp:88
    #2 CashPosition::apply(...) cash_position.cpp:214
  Mutex M1 acquired here while holding mutex M2 in thread T7:
    #1 CashDrawer::release(...) cash_drawer.cpp:57
    #2 SessionSweeper::expire(...) sweeper.cpp:143
```

---

## 4. Under the hood (senior-level)

**What a bank does with your RCA.** The application owner reads the summary and impact; IT risk
checks that the root cause is distinguished from the trigger and that preventive actions are
owned and dated; the architecture team reads section 4 for whether the explanation is
technically credible; audit later checks that the actions were closed. Regulators in many
jurisdictions expect banks to report significant IT incidents and to show root-cause analysis and
remediation; your document is the source for that. Write as if each section will be read by a
different, sceptical person - because it will.

**Causal models beyond 5-whys.** 5-whys is linear; real incidents are usually a combination of
conditions (a defect, a missing check, a trigger, slow detection). Two tools seniors use:

- **Fishbone (Ishikawa) diagram**: branches for code, design, process, testing, environment,
  people/communication; list causes on each. Good for finding contributing factors.
- **Fault tree / "necessary and sufficient"**: list the conditions that were *all* necessary
  (remove any one and the incident does not happen). Each necessary condition is a place a
  preventive action can break the chain. In RCA 2: the opposite lock order (defect), the overlap
  (trigger), the same session at the same moment (timing), no watchdog (slow detection). Breaking
  the first prevents every future occurrence; breaking the second only this one.

**Why "proven" is a high bar for memory corruption.** The crash site is not the bug site (module
03), so the dump alone rarely proves the cause. The chain usually needs: the dump showing *what*
was corrupted (header bytes, freed-pattern), correlation showing *which input* preceded it, and a
reproduction under ASan/PageHeap showing *which line* wrote it. Two of the three give a strong
inference; all three give proof. Say which you have.

**Why "proven" is achievable for deadlocks.** A deadlock is a static state; the dump shows every
thread, every lock owner and the cycle. Two dumps prove that nothing moves. That is why the
deadlock RCA can state the root cause as proven within an hour, while a heap-corruption RCA often
needs a reproduction first.

**Preliminary versus final.** Issue a preliminary RCA when the contract requires it, even if the
root cause is not yet proven: impact, timeline, mitigation, current hypothesis clearly labelled,
and what you are doing to confirm it. The final RCA replaces it; keep a version history so the
reader can see what changed and why.

## 5. In production

- **Start the RCA during the incident.** The incident log of module 11 (timestamped actions and
  observations) becomes section 3 almost unchanged. Reconstructing a timeline from memory a week
  later is unreliable and slow.
- **Have someone who did not work the incident review it.** The reviewer checks: can the summary be
  understood without the rest; is every root-cause claim backed by appendix evidence; is the
  trigger separated; does each preventive action have owner, date and a "done when".
- **The "could not reproduce" RCA.** Sometimes you cannot prove the root cause. The honest version:
  evidence gathered, most likely mechanism with its supporting evidence, what else was ruled out
  and how, mitigation in place, *diagnostics added* so the next occurrence will be conclusive, and
  the RCA stays open. Banks accept this far more readily than a confident wrong answer that fails
  a month later.
- **Repeat incidents.** Before writing, search previous RCAs for the same stack signature or
  component. If this is a recurrence, say so in the summary and explain why the previous fix did
  not hold. Hiding it is found out in the client's quarterly review.
- **Third-party causes.** If the bug is in a library, OS or database, prove it (a minimal
  reproduction, the vendor's bug id). Even then, the RCA usually has a part you own: why your
  system was exposed, why there was no containment.
- **What you leave out.** Internal blame, speculation not labelled as such, customer personal data
  (mask account numbers in evidence), and full dumps (reference them by id and hash; they stay in
  the secure store).
- **Shakir's context.** For Extrieve-style deployments at HDFC Bank or Kotak Mahindra Bank, the bank
  usually has its own RCA template and review board. Map this module's sections onto their
  template; the content requirements are the same. Keep the WinDbg excerpts short (the 6 lines
  that prove it, not 200 lines of `!analyze -v`).

## 6. Try it

1. **Easy - trigger or root cause.** For each, label trigger, root cause or symptom, and rewrite
   the wrong ones: "server ran out of file descriptors"; "a new partner sent an empty amount field";
   "connections were not closed on the error path"; "Windows patch Tuesday"; "an `unordered_map`
   was modified by two threads without a lock". Done when each has a label and a one-line
   justification using the "will it survive tomorrow" test.
2. **Easy - critique a summary.** Rewrite this summary for a bank: "Due to an unexpected issue the
   xferd module crashed several times. The developer has fixed it. Root cause was high load. We
   apologise for any inconvenience caused and regret the same." Done when the rewrite states what
   failed, impact with numbers, the root cause, trigger, fix and data integrity in under 8
   sentences.
3. **Medium - RCA from the course's deadlock.** Run `m10_deadlock`, take two `gcore`s (module 11
   evidence script), and write sections 4 (root cause with evidence chain), 5, 7 and 10 of an RCA
   for it, with your real LWPs and stacks in an appendix. Done when every claim in section 4 is
   labelled proven, inferred or hypothesis and points to an appendix item.
4. **Medium - RCA from the heap overflow.** Using `m04_heap_overflow` (no ASan core plus an ASan
   reproduction), write a complete RCA following RCA 1 as a template, inventing a plausible
   trigger. Done when a reviewer can find the root cause, the trigger and three preventive actions
   that are specific, owned, dated and verifiable.
5. **Hard - the unprovable one.** Write a preliminary RCA for `m10_data_race` as if you had only
   one core showing a crash in `_Rb_tree_insert_and_rebalance` and no reproduction yet. Done when
   the document makes no unproven claim as fact, lists what was ruled out, and names the
   diagnostic you added to make the next occurrence conclusive.
6. **Hard - your own incident.** Take a real incident from your work (anonymise the bank, people
   and numbers) and rewrite its RCA in this structure. Done when you can present it in 5 minutes
   in an interview, including the trigger-versus-root-cause distinction and one preventive action
   that would have stopped it.

## 7. Interview questions

**Q: What is the difference between the trigger and the root cause?**
A: The trigger is the event that exposed the problem at that time - a traffic spike, an unusual
input, a schedule overlap. The root cause is the defect that made the system fail under that
event; removing it makes the trigger harmless. Test: if the trigger recurs tomorrow, will the
system survive? If not, you have only described the trigger.

**Q: Walk me through the structure of an RCA you would send a bank.**
A: Header, executive summary in plain words, impact with numbers and data-integrity statement,
timestamped timeline with sources, root cause with a labelled evidence chain, trigger, contributing
factors, 5-whys down to the process gap, resolution (mitigation and permanent fix), owned and dated
preventive actions, verification of the fix and of its holding in production, and an appendix of
evidence excerpts.

**Q: How do you write an RCA when you could not prove the root cause?**
A: Issue it as preliminary: state the evidence, the most likely mechanism labelled as a hypothesis,
what was ruled out and how, the mitigation in place, and the diagnostics added so the next
occurrence is conclusive. Keep it open with a review date. A confident wrong root cause costs far
more credibility when the incident recurs.

**Q: What makes a preventive action good?**
A: It is specific, owned, dated and verifiable, and it addresses the process gap, not only the
bug: eliminating the bug class, detecting it earlier, containing it or diagnosing it faster. "Add a
libFuzzer target for the parser to nightly CI by 30 Nov, owner X, done when it runs" is an action;
"improve testing" is not.

**Q: Why does a heap-corruption RCA usually need a reproduction while a deadlock RCA does not?**
A: The heap corruption is detected later, in other code, so the dump shows where the damage was
found, not who did it; you need correlation plus an ASan or PageHeap reproduction to prove the
writing line. A deadlock is a frozen state: the dump shows every waiter, every owner and the cycle,
and two dumps prove nothing is moving.

**Q: What tone do you use with a banking client?**
A: Factual, calm and owning: precise numbers and timestamps with timezone, "a defect in our code",
no internal names or blame, one sentence of acknowledgement rather than repeated apology, plain
words in the summary and precision in the technical section, and an explicit statement about
whether any transaction was lost, duplicated or mis-posted and how that was verified.

**Q: How far should the 5-whys go?**
A: Past the bug, to the process that let it ship: why review, tests and tooling did not catch it.
Stop when the next answer is outside your control or is a process you can change. Never end at a
person; "a developer made a mistake" is always followed by "why did the system allow it to reach
production?".

## 8. Recap

- An RCA: header, summary, impact, timeline, root cause with evidence, trigger, contributing
  factors, 5-whys, resolution, preventive actions, verification, appendix.
- The trigger is why it happened that day; the root cause is why the system could fail at all.
  Test: will it survive the trigger tomorrow?
- Label every claim as proven, inferred or hypothesis; evidence goes in the appendix.
- Technical root cause plus process root cause: the bug and why it shipped.
- Banking tone: factual, owning, blameless, numbers and timezones, explicit data-integrity
  statement, plain summary.
- Preventive actions are specific, owned, dated, verifiable; classify as eliminate, detect,
  contain, diagnose.
- Verification has two parts: the fix fixes it (fails before, passes after) and the fix holds
  (a metric over a period including high-risk windows).
- Heap corruption RCAs need correlation plus reproduction; deadlock RCAs are proven by two dumps.
- Preliminary RCAs are honest about uncertainty and add diagnostics; they stay open.
