# 13. High availability and operations

**Time:** ~2.5 hours   **Prerequisites:** 09, 10, 11, 12

## 1. In plain words

**High availability (HA)** means the system keeps serving when parts of it fail or are being
changed. Nothing is up 100% of the time; HA is about agreeing how much downtime and data loss is
acceptable, designing so a single failure does not cause either, and practising the recovery so
it works at 3 AM. **Operations** is the day-to-day work that keeps it so: deploying without
outages, backing up and actually restoring, testing failure on purpose, and writing down what to do
when something breaks.

Analogy: a bank branch. Availability is "the branch is open when customers come". A second teller
means one sick teller does not close the branch (redundancy). A generator means a power cut does
not close it (no single point of failure). A fire drill means people know where to go (chaos
testing and runbooks). Renovating one counter at a time while the others serve customers is a
rolling deploy. Moving to a new building over a weekend without customers noticing is a
zero-downtime cutover, which is on your resume, and section 5 gives you the words for it.

This module is mostly vocabulary and judgement, with numbers. Interviewers ask about it to see
whether you have run systems or only written them.

## 2. The real thing

### 2.1 SLA, SLO, SLI and error budgets

- **SLI** (service level indicator): a measured number. "Fraction of posting RPCs that succeeded
  in under 500 ms over the last 5 minutes." Defined from the user's point of view: success *and*
  latency, measured where the user sees it (the client or load balancer, not the server's own log).
- **SLO** (service level objective): the target for an SLI. "99.9% of postings succeed within
  500 ms, measured over 30 days." Internal, chosen by the team.
- **SLA** (service level agreement): a contract with a customer, with penalties (service credits)
  if missed. Always looser than the SLO so you have warning before you pay. A bank's contract with
  a core-banking vendor typically has availability during business hours and RTO/RPO clauses.
- **Error budget**: 100% minus the SLO. At 99.9% over 30 days you may be "bad" for 43.2 minutes. If
  the budget is healthy, ship features and take deploy risk; if it is spent, freeze risky changes
  and fix reliability. It turns "is this reliable enough?" into arithmetic.

The nines, per 30-day month and per year (pure arithmetic):

| Availability | Downtime per 30 days | Downtime per year |
|--------------|----------------------|-------------------|
| 99% | 7.2 hours | 3.65 days |
| 99.9% | 43.2 minutes | 8.76 hours |
| 99.95% | 21.6 minutes | 4.38 hours |
| 99.99% | 4.32 minutes | 52.6 minutes |
| 99.999% | 26 seconds | 5.26 minutes |

**Composition.** Components in **series** (request needs all of them) multiply: an app at 99.9%
calling a database at 99.9% gives at most 99.8%. Components in **parallel** (any one suffices)
combine as `1 - (1-a)(1-b)`: two independent 99% instances give 99.99%, *if* failures are truly
independent and failover is instant, which they never fully are. This is why you remove single
points of failure and why shared dependencies (one DNS, one config service) quietly cap everything.

### 2.2 RTO and RPO

- **RTO** (recovery time objective): how long the service may be down after a disaster. "Back
  within 15 minutes."
- **RPO** (recovery point objective): how much recent data may be lost, measured in time. "At most
  the last 5 seconds of commits." RPO = 0 means no committed transaction may ever be lost.

```
   last good backup / replicated point        disaster              service restored
 ---------|-------------------------------------X---------------------------|------>
          |<---------- data at risk (RPO) ----->|<------ downtime (RTO) ---->|
```

What determines them:

| Mechanism | Typical RPO | Typical RTO |
|-----------|-------------|-------------|
| Nightly backup, restore to new hardware | up to 24 h | hours |
| Backup + continuous WAL archiving (PITR) | seconds to minutes | tens of minutes to hours (restore + replay) |
| Async standby, manual promotion | seconds (replication lag) | minutes (human in loop) |
| Async standby, automatic failover (Patroni) | seconds | ~30 s to a few minutes |
| Sync standby / Raft quorum (YugabyteDB RF3) | 0 | seconds (leader election) |
| Cross-region async (xCluster) for region loss | seconds | minutes (switch apps to the other cluster) |

(Typical figures, not guarantees; measure yours in a drill.) Banks and regulators usually specify
both for each system tier. Know your employer's numbers.

### 2.3 Redundancy patterns: active-passive vs active-active

**Active-passive** (primary-standby, hot/warm/cold standby): one instance serves; another is ready
to take over.
- **Hot** standby: running, data replicated continuously, takes over in seconds to minutes.
- **Warm**: running, data partly current, needs some steps. **Cold**: hardware or images only;
  restore from backup.
- Pros: simple, no write conflicts. Cons: idle capacity, failover is a rarely-exercised path that
  breaks when you need it, split-brain risk without fencing (module 10).

**Active-active**: all instances serve traffic.
- **Stateless tiers** (app servers behind a load balancer) are naturally active-active: lose one,
  the others absorb its load. Requires headroom: N instances at 60% load survive one loss only if
  the others can take the extra share (N=3 at 60% each becomes 2 at 90%).
- **Stateful tiers** are harder. A consensus database (YugabyteDB) is active-active at the tablet
  level: every node leads some tablets. Multi-leader across regions brings write conflicts (module
  10 section 2.6).
- **N+1 / N+2 capacity**: size so the loss of one (or two) instances still meets the SLO.

**Failure domains**: process, host, rack, availability zone, region. Place replicas so that one
domain failing never takes out a majority. Three zones with one replica each is the standard RF3
layout.

### 2.4 Deploying without downtime

**Rolling deploy**: replace instances one (or a few) at a time. For each: remove from the load
balancer (fail readiness, wait the detection window), drain in-flight requests, stop, deploy,
start, pass health checks, re-add. Needs: the old and new versions can run **side by side**
(protocol and schema compatibility), and enough spare capacity for one instance being out.

**Blue-green deploy**: run two complete environments. Blue serves production; deploy to green,
test it, then switch traffic (load balancer target, DNS, or VIP) from blue to green in one step.
Rollback is switching back. Costs double capacity during the change. The database is usually
**shared** between blue and green, which is the hard part: the schema must work for both.

**Canary release**: send a small fraction of traffic (1%, then 10%, then 50%) to the new version
and compare its error rate and latency against the old. Automatic rollback if the canary is worse.
Needs per-request routing (L7) or a subset of instances.

**Feature flags**: deploy code dark, turn the feature on separately (per branch, per percentage).
Decouples deploy from release; rollback is a config change.

**Schema changes without downtime: expand and contract.**

```
 1. Expand:   add the new column/table (nullable, no default rewrite). Old code ignores it.
 2. Migrate:  deploy code that writes both old and new; backfill old rows in batches.
 3. Switch:   deploy code that reads the new column.
 4. Contract: after every instance runs the new code (and rollback is no longer needed),
              stop writing the old column, then drop it.
```

Never rename or drop a column in the same release that stops using it; a rolling deploy has old
instances still reading it. In distributed SQL, prefer online index builds (YugabyteDB builds
indexes online with backfill; `CREATE INDEX NONCONCURRENTLY` exists for empty tables).

**Your RPC server and rolling deploys**: long-lived branch connections make draining slow. Tell
clients to reconnect elsewhere (a protocol-level "server going away" message, like HTTP/2 GOAWAY),
stop accepting new requests on the connection, finish in-flight ones, then close. Clients must
reconnect with jittered backoff (module 09) or every branch reconnects to the same survivor at once.

### 2.5 Database HA: YugabyteDB RF3 losing a node

Setup: three yb-tservers in three zones, RF3, three yb-masters. Each tablet has three peers, one
per node; leaders spread evenly.

What happens when node 2 dies (power loss):

1. **Tablets led by node 2**: their followers stop receiving heartbeats. After the leader lease
   expires (about 2 s by default) and an election timeout, a follower with an up-to-date log wins.
   Writes and strong reads for those tablets fail or wait for a few seconds (typical), then
   resume. Clients see errors or retries; YugabyteDB's query layer retries some internally.
2. **Tablets led elsewhere**: lose one follower. Commits still need 2 of 3, which the remaining
   two provide. No visible effect apart from slightly higher tail latency.
3. **Data loss**: none. Every committed write was on a majority, so at least one survivor has it
   and the election rule guarantees it is the new leader's. RPO = 0.
4. **Fault tolerance now**: every tablet is down to 2 replicas. A second failure loses quorum for
   tablets with peers on both dead nodes. You are **under-replicated**.
5. **Re-replication**: if node 2 stays down beyond `follower_unavailable_considered_failed_sec`
   (default 900 s; check your version), the masters re-create the missing replicas on other nodes,
   **if there are any** (needs a fourth node, or a replacement). With exactly three nodes, nothing
   can move; you run at reduced tolerance until node 2 or a replacement returns.
6. **Node returns within the window**: its peers catch up from the leaders' logs (or remote
   bootstrap if too far behind). The load balancer moves some leaders back to it.
7. **yb-master** on node 2: one of three masters lost; the master Raft group keeps quorum.
   Catalog operations continue.
8. **Clients**: drivers with the node in their connection list get connection errors to it and
   must use the others. YugabyteDB's smart drivers (cluster-aware) or a load balancer in front of
   YSQL handle this; a single hard-coded host in your app server's config is a single point of
   failure that defeats RF3.

Contrast **single-region RF3** with **zone loss**: same as node loss if one node per zone. **Region
loss** needs either a stretched cluster across three regions (every write pays inter-region
latency) or xCluster async replication to a second cluster (RPO > 0, failover is an application
switch).

PostgreSQL with Patroni: primary dies -> Patroni's leader key in etcd expires (TTL, typically
tens of seconds) -> a standby with the least lag is promoted -> the VIP or HAProxy target moves ->
old primary, when it returns, is rewound (`pg_rewind`) and rejoins as standby. With async standbys
RPO is the lag at the moment of failure; with synchronous standbys it is 0 at the cost of commit
latency.

### 2.6 Backups

**Replication is not a backup.** A `DELETE` without a `WHERE`, a bad migration, or ransomware is
replicated to every copy within milliseconds. Backups protect against **logical** damage and
operator error; replication protects against **hardware** loss.

- **Types**: logical (`pg_dump`/`ysql_dump`: portable, slow for big data, consistent snapshot);
  physical (file or snapshot level: fast, same version); continuous (WAL archiving / point-in-time
  recovery to any second within a retention window).
- **YugabyteDB**: distributed snapshots (consistent across tablets at one hybrid time), exported
  to object storage; **PITR** via snapshot schedules plus retained history lets you restore a
  database to a time before a bad change. Also flashback-style reads at an older time within the
  history retention.
- **3-2-1 rule**: three copies, two different media, one off-site (now often: one immutable or
  air-gapped copy, against ransomware).
- **Restore testing**: a backup that has never been restored is a hope, not a backup. Schedule
  restores into a scratch environment, verify row counts and checksums (your parity suite idea),
  and time it: the restore duration is your real RTO for logical disasters.
- **Retention** follows regulation (banks often must keep years of records) and the time it takes
  to notice a problem (logical corruption is often found days later).

### 2.7 Chaos testing and game days

**Chaos engineering**: deliberately inject failures in a controlled way to find weaknesses before
they find you. Netflix's Chaos Monkey (randomly terminating instances in production) made it
famous. Principles: define steady state (SLIs), hypothesise it holds under a failure, inject the
failure with a small blast radius, observe, and fix what breaks.

Failures to inject, from cheap to bold:
- kill a process (`kill -9`), reboot a node, stop a container;
- network: latency, loss, partitions (`tc netem`, `iptables` drops) between app and DB, between DB
  nodes;
- disk full, slow disk, read-only filesystem;
- clock skew (step a node's clock; YugabyteDB should refuse or restart reads, module 10);
- dependency outage (stop the SMS gateway stub; does posting still work?);
- CPU or memory pressure (`stress-ng`).

**Jepsen** (Kyle Kingsbury) is the rigorous version for databases: a test harness that runs
operations against a cluster while injecting partitions and clock faults, then checks the history
against a consistency model. Its public analyses (including YugabyteDB's in 2019) are excellent
reading for how distributed databases fail.

**Game day**: a scheduled exercise where the team runs a failure scenario (fail over the
database, lose a zone) in staging or production, following the runbook, with a time-keeper and a
scribe. You find out that the runbook is out of date, the alert went to the wrong channel, and the
failover takes 11 minutes instead of 1.

### 2.8 Runbooks and incident response

A **runbook** is a step-by-step procedure for a known operational task or alert: what it means,
how to confirm, what to do, how to verify, how to roll back, whom to escalate to. Good runbooks:

- start from the alert name (the on-call engineer arrives from the page);
- give exact commands with placeholders, not prose ("run `yb-admin ... list_tablets`");
- include "how to tell if this made things worse" and a rollback;
- are tested in game days and updated after every incident.

**Incident response** basics: one **incident commander** coordinates (does not debug); one
communicator updates stakeholders; others investigate. **Mitigate first, root-cause later**: roll
back, fail over, shed load. Keep a timeline. Afterwards, a **blameless postmortem**: what happened,
impact, timeline, contributing causes (plural), what went well, action items with owners. Module
`05_Memory_Debugging_RCA/` covers root-cause analysis technique.

**Key metrics for HA**: MTTD (mean time to detect), MTTR (mean time to recover/repair), MTBF (mean
time between failures). Availability is about MTBF / (MTBF + MTTR): you raise it more cheaply by
shrinking MTTR (fast detection, practised recovery) than by preventing every failure.

## 3. Code

There is no new program for this module; the operational tools are commands. A practice session
on a local three-node YugabyteDB (Docker or WSL; see `07_Databases_Internals/11_yugabytedb_and_distributed_sql.md`
for setup):

```bash
# Three nodes on one machine with yugabyted (loopback aliases 127.0.0.1-3 work on Linux).
yugabyted start --advertise_address=127.0.0.1 --base_dir=/tmp/yb1
yugabyted start --advertise_address=127.0.0.2 --base_dir=/tmp/yb2 --join=127.0.0.1
yugabyted start --advertise_address=127.0.0.3 --base_dir=/tmp/yb3 --join=127.0.0.1
yugabyted status --base_dir=/tmp/yb1

# Steady load: a loop of small transactions with timing (psql via ysqlsh).
ysqlsh -h 127.0.0.1 -c "CREATE TABLE t (k int PRIMARY KEY, v int)"
while true; do
  start=$(date +%s%N)
  ysqlsh -h 127.0.0.1 -qAt -c "INSERT INTO t VALUES ($RANDOM, 1) ON CONFLICT (k) DO UPDATE SET v = t.v + 1" \
    || echo "$(date +%T) ERROR"
  echo "$(date +%T) $(( ($(date +%s%N) - start) / 1000000 )) ms"
  sleep 0.2
done

# In another terminal: kill a node that is not the one the client talks to, and watch the latency
# column for the few-second blip while its tablets elect new leaders.
yugabyted stop --base_dir=/tmp/yb2
# Bring it back and watch it catch up.
yugabyted start --base_dir=/tmp/yb2
```

The Raft simulation from module 11 (`code/m11_raft_election_sim.cpp`) shows the same events at
the protocol level: crash, election, catch-up after restart.

For application-tier rolling restarts, module 05's tcpkv server already implements graceful
shutdown (stop accepting, drain, deadline). Module 09 section 2.5 explains readiness checks.

## 4. Under the hood (senior-level)

**Why failover takes as long as it does.** Detection (missed heartbeats times a threshold) plus
election or promotion plus client rediscovery (DNS TTL, connection pool eviction, driver retry
backoff). In YugabyteDB the leader lease bounds detection from below: followers must not elect a
new leader while the old one might still hold a valid lease, or two nodes could serve reads. With
Patroni, the etcd key TTL and loop wait set it. On the client side, the slowest part is often a
connection pool holding dead connections until each one times out: set TCP keepalive and
driver-level timeouts (module 02 and 09) so dead connections are discovered in seconds.

**Gray failure.** The hardest failures are partial: a node that answers health checks but serves
requests slowly, a NIC dropping 2% of packets, a disk with 500 ms tail latency. Health checks say
"up", users say "slow". Defences: health checks that measure real work latency, outlier detection
on the client side (eject the slowest backend), hedged requests for reads, and SLO alerts measured
from the client side.

**Correlated failures defeat redundancy math.** Two replicas on the same hypervisor, the same bad
config pushed everywhere, the same certificate expiring on every node at midnight, the same leap
second bug in every kernel. Independence is the hidden assumption in every "two 99% components
give 99.99%" calculation. Stagger config rollouts, use different failure domains, and track
expiry dates.

**Capacity during failure.** With RF3 on three nodes each node holds a third of the leaders. Losing
one moves its leaders to the two survivors: each now does 50% of the leader work instead of 33%,
a 1.5x load increase. If they were at 70% CPU, they are now overloaded and the failure cascades.
Size for the failure case (N+1) and test it.

**Zero-downtime is a property of the protocol, not just the infrastructure.** Old and new
versions must interoperate during a rollout: the wire protocol needs versioning (module 07: version
byte, optional trailing fields), the schema must be compatible both ways (expand/contract), and
serialized data in queues or caches must be readable by both. Most "the deploy caused an outage"
incidents are compatibility failures, not infrastructure failures.

## 5. In production

### 5.1 Failure stories to learn from

- **Failover that had never been tested.** The standby's replication had silently broken weeks
  earlier (disk full on the standby). When the primary failed, the standby was weeks behind.
  Monitoring replication lag, and alerting when replication *stops* rather than only when lag is
  high, would have caught it.
- **Backups that could not be restored.** Nightly dumps succeeded for a year; the first restore
  attempt found they excluded a schema added later. Restore drills catch this.
- **The rollback that wasn't.** A release included a migration that dropped a column; rollback of
  the binaries brought back code that read the dropped column. Expand/contract would have allowed
  rollback.
- **Single hard-coded DB host.** The app server's connection string pointed at one YugabyteDB
  node. That node was rebooted for patching; the whole application was down although the cluster
  was healthy. Use all nodes in the connection string, a cluster-aware driver, or a load balancer.

### 5.2 Vocabulary for explaining a zero-downtime cutover

Your resume says "zero-downtime cutover to multi-node production". Interviewers will ask "how?",
and the answer should use precise terms. The words, each with a one-line meaning:

- **Bulk load (initial load, snapshot copy)**: copy all existing data to the new store while the
  old one keeps serving.
- **Change data capture (CDC) / delta sync / catch-up**: apply changes made on the old store since
  the bulk load began, identified by a change log, modification timestamp or sequence number.
- **Dual write**: application writes to both stores for a period (risky: partial failures make
  them diverge; needs reconciliation).
- **Shadow reads / dark reads**: read from both and compare results without showing the new one to
  users.
- **Parity check / reconciliation**: compare old and new record by record (counts, checksums,
  field-level comparison) to prove equivalence. Your data-parity regression suite.
- **Go/no-go criteria**: explicit conditions that must hold before the switch (parity zero
  mismatches, lag under N seconds, smoke tests pass).
- **Freeze / quiesce / brief read-only window**: stop writes for the seconds needed to apply the
  final delta, so old and new are identical at the switch.
- **Cutover (flip, switchover)**: point the application at the new store (config change, VIP, DNS,
  connection string), usually via a rolling restart of app servers.
- **Rollback plan**: how to switch back, and how to carry writes made on the new store back to the
  old one if you do. Time-boxed decision point: after it, roll forward only.
- **Hypercare**: the period after cutover with extra monitoring and people on call.

A template answer, to be filled with your real numbers (see
`11_Interview_Prep_Job_Search/02_telling_your_story.md`; never invent figures in an interview):

> "We migrated `[N]` deployments from Berkeley DB to a three-node YugabyteDB cluster, RF3 across
> `[zones/hosts]`. The engine did a bulk load while the old system kept running, then a delta pass
> for records changed since the load, identified by `[mechanism]`. A parity suite compared old and
> new field by field; zero mismatches was a go/no-go criterion. The switch itself was `[a config
> change plus rolling restart of the app servers / a short read-only window of N seconds]`, done in
> an agreed window with a written runbook and a rollback path that we rehearsed in UAT. Downtime as
> seen by branches was `[none / under N seconds]`. Afterwards we monitored `[metrics]` during
> hypercare."

Then expect the pushback: "What if a write arrived during the final delta?" (the freeze or
read-only window, or CDC that keeps applying until the flip); "What if the parity check found a
mismatch on the night?" (no-go, fix, next window); "How would you roll back after branches had
posted to the new store?" (reverse sync or replay of the new store's changes, or a defined
point-of-no-return after which you only roll forward); "What does RF3 buy you that Berkeley DB
did not?" (survives a node loss with RPO 0 and seconds of RTO; module 11).

## 6. Try it

1. **(easy)** Compute the monthly error budget in minutes for 99.5%, 99.9% and 99.95%, and the
   availability of a request path of load balancer (99.99%), app tier (99.95%) and database
   (99.99%) in series. Done when: the numbers are right and you can say which component to improve
   first.

2. **(easy)** Write the RTO and RPO you believe your employer's banking system has today, and
   what mechanism provides each. Mark unknowns. Done when: you have a list of questions to ask the
   operations team.

3. **(medium)** Run the three-node yugabyted exercise in section 3. Kill a node under load and
   measure the longest latency blip and the number of errors. Done when: you can explain the blip
   with leader leases and elections, and the absence of data loss with Raft majorities.

4. **(medium)** Write an expand/contract plan for renaming `account.acct_type` to
   `account.product_code` in a system with four app servers under rolling deploys. Done when: each
   step lists the code version, the schema state, and why a rollback at that step is safe.

5. **(hard)** Write a runbook for "YugabyteDB node down" in your format: alert, impact, confirm
   (commands), immediate actions, when to replace the node, how to verify re-replication finished,
   rollback, escalation. Done when: a colleague who has never seen YugabyteDB could follow it.

6. **(hard)** Run a small chaos experiment on the module 05 tcpkv server: three instances behind
   HAProxy in TCP mode, the load tester running, then `kill -9` one instance and later add 200 ms
   latency to another with `tc netem`. Done when: you have recorded the error count and p99 for
   each fault and one change that improved it (health check interval, client retry, timeout).

## 7. Interview questions

**Q: SLA, SLO, SLI?**
A: The SLI is the measured indicator (fraction of requests that succeed within 500 ms); the SLO is
the internal target for it (99.9% over 30 days); the SLA is the external contract with penalties,
set looser than the SLO. The gap between 100% and the SLO is the error budget that governs release
risk.

**Q: RTO versus RPO?**
A: RTO is how long you may be down after a failure; RPO is how much recent data you may lose,
measured in time. Synchronous or quorum replication gives RPO 0; async replication gives RPO equal
to the lag; backups give RPO equal to the backup interval unless you archive the WAL.

**Q: Active-passive versus active-active?**
A: Active-passive keeps a standby ready to take over: simple, but idle capacity and a rarely
exercised failover path with split-brain risk. Active-active serves from all instances: better
utilisation and no failover step for stateless tiers, but needs headroom for a loss and, for
stateful tiers, either consensus or conflict handling.

**Q: Rolling versus blue-green deploys?**
A: Rolling replaces instances a few at a time, cheap but old and new versions coexist so they must
be compatible. Blue-green deploys to a full parallel environment and switches traffic at once, with
instant rollback, at double capacity; the shared database still needs compatible schema changes.

**Q: What happens when a YugabyteDB RF3 cluster loses one node?**
A: Tablets led by that node elect new leaders among their two surviving peers after the leader
lease expires, so their writes pause for a few seconds; other tablets are unaffected. No committed
data is lost because every commit was on a majority. The cluster is under-replicated until the node
returns or, after a timeout, replicas are re-created on other nodes if any exist.

**Q: Why is replication not a backup?**
A: Replication copies every change, including mistakes and malicious deletes, to every replica
almost instantly. Backups and point-in-time recovery let you return to a state before the damage.
Restores must be tested regularly to count.

**Q: How do you change a schema with zero downtime?**
A: Expand and contract: add new structures compatible with old code, deploy code that writes both
and backfill, switch reads to the new structure, and only after the old code is gone and rollback
is no longer needed, remove the old structure.

**Q: How did you achieve a zero-downtime cutover?**
A: Bulk load while the old system served, delta catch-up of changes since, a parity check as a
go/no-go gate, a short quiesce for the final delta, a configuration flip with rolling restarts,
a rehearsed rollback plan, and hypercare monitoring. Give your real numbers.

## 8. Recap

- SLI measures, SLO targets, SLA contracts; error budget = 100% - SLO. 99.9% = 43.2 min/month.
- Series availabilities multiply; parallel ones help only if failures are independent.
- RTO = downtime allowed, RPO = data loss allowed. Quorum replication gives RPO 0.
- Active-passive vs active-active; failure domains; N+1 capacity.
- Rolling, blue-green, canary, feature flags; expand/contract for schemas; protocol versioning.
- YugabyteDB RF3 node loss: seconds of unavailability for that node's leaders, no data loss,
  under-replicated until replaced; never hard-code one DB host.
- Replication is not backup; test restores; 3-2-1; PITR.
- Chaos testing and game days find the broken failover before a real failure does.
- Runbooks start from the alert and give exact commands; mitigate first; blameless postmortems.
- Cutover vocabulary: bulk load, delta/CDC, parity check, go/no-go, quiesce, flip, rollback, hypercare.
