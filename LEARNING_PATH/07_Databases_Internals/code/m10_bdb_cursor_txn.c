/* m10_bdb_cursor_txn.c : the Berkeley DB C API in one program (module 10).
 *
 * Build and run on WSL Ubuntu 24.04 (libdb 5.3):
 *     sudo apt install -y libdb-dev
 *     gcc -Wall -Wextra -O2 m10_bdb_cursor_txn.c -o bdbdemo -ldb
 *     ./bdbdemo            (run it twice: the second run shows DB_KEYEXIST and recovery at open)
 *
 * Then inspect what the library left on disk:
 *     ls -la bdb_env/                      __db.001.. shared regions, log.0000000001, users.db
 *     db_stat -h bdb_env -c                lock statistics (lockers, deadlocks, waits)
 *     db_stat -h bdb_env -m                mpool: cache hits and misses
 *     db_dump -h bdb_env -p -s users users.db | head
 *
 * What it shows, in order:
 *   1. an environment with all four subsystems (mpool, lock, log, txn) and DB_RECOVER at open
 *   2. a BTREE primary keyed by a packed (branch, user_code) struct
 *   3. a DUPSORT secondary keyed by branch, maintained by the library through a key extractor
 *   4. inserts in one transaction with DB_NOOVERWRITE
 *   5. a cursor range scan with DB_SET_RANGE / DB_NEXT, updating through the cursor (DB_CURRENT)
 *   6. a secondary-index lookup with pget and DB_NEXT_DUP
 *   7. the deadlock retry wrapper every transactional Berkeley DB program needs
 *   8. a checkpoint (your job, not the library's) and a clean close in the right order
 */
#define _DEFAULT_SOURCE            /* usleep, and the BSD u_int types db.h relies on */
#include <db.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define N_USERS        5000
#define N_BRANCHES     20
#define STATUS_ACTIVE  1
#define STATUS_LOCKED  2

/* Every call returns 0 or an error code; on error remember it in `ret` and jump to cleanup. */
#define CHECK(expr, what) do { int rc_ = (expr); if (rc_ != 0) { ret = rc_; fprintf(stderr, "%s: %s\n", what, db_strerror(rc_)); goto fail; } } while (0)

/* Packed so the on-disk bytes are exactly the fields: no padding garbage in keys (a padded key
 * with stack garbage in the pad bytes would make "equal" keys compare unequal). Key fields are
 * fixed-width text, so the default lexicographic BTREE comparison gives branch-then-user order. */
#pragma pack(push, 1)
struct user_key { char branch[6]; char user_code[9]; };                 /* 15 bytes */
struct user_rec {
    uint32_t id;
    uint8_t  status;
    uint16_t failed_attempts;
    int64_t  last_login_epoch;
    char     name[32];
};                                                                       /* 47 bytes */
#pragma pack(pop)

/* Called by the library for every put/delete on the primary: fill `result` with the secondary key.
 * result->data may point into pkey: it is valid for the duration of the call. */
static int branch_key_extractor(DB *secondary, const DBT *pkey, const DBT *pdata, DBT *result)
{
    (void)secondary; (void)pdata;
    const struct user_key *k = pkey->data;
    memset(result, 0, sizeof *result);
    result->data = (void *)k->branch;
    result->size = sizeof k->branch;
    return 0;                                  /* or DB_DONOTINDEX to leave this record out */
}

static void fill(struct user_key *k, struct user_rec *r, int i)
{
    char tmp[16];
    memset(k, 0, sizeof *k);
    memset(r, 0, sizeof *r);
    snprintf(tmp, sizeof tmp, "BR%04d", i % N_BRANCHES);
    memcpy(k->branch, tmp, sizeof k->branch);  /* 6 chars, no NUL stored */
    snprintf(tmp, sizeof tmp, "U%08d", i);
    memcpy(k->user_code, tmp, sizeof k->user_code);
    r->id = (uint32_t)i;
    r->status = (i % 7 == 0) ? STATUS_LOCKED : STATUS_ACTIVE;
    r->failed_attempts = (r->status == STATUS_LOCKED) ? 5 : 0;
    r->last_login_epoch = 1700000000 + i;
    snprintf(r->name, sizeof r->name, "User %d", i);
}

/* ---- the retry wrapper: on DB_LOCK_DEADLOCK abort and run the WHOLE unit of work again ---- */
typedef int (*work_fn)(DB_TXN *, void *);

static int run_txn(DB_ENV *env, work_fn work, void *arg)
{
    for (int attempt = 0; attempt < 10; ++attempt) {
        DB_TXN *t = NULL;
        int ret = env->txn_begin(env, NULL, &t, 0);
        if (ret) return ret;
        ret = work(t, arg);
        if (ret == 0) return t->commit(t, 0);
        t->abort(t);                              /* releases every page lock the txn held */
        if (ret != DB_LOCK_DEADLOCK && ret != DB_LOCK_NOTGRANTED) return ret;
        fprintf(stderr, "deadlock, retry %d\n", attempt + 1);
        usleep(1000u << attempt);                 /* 1 ms, 2 ms, 4 ms ... */
    }
    return DB_LOCK_DEADLOCK;
}

/* A unit of work: reset failed_attempts for one user. DB_RMW takes the write lock on the read,
 * which avoids the classic read-lock-then-upgrade deadlock between two such transactions. */
struct reset_arg { DB *dbp; const char *branch; const char *user_code; };

static int reset_attempts_work(DB_TXN *txn, void *p)
{
    struct reset_arg *a = p;
    struct user_key k;
    struct user_rec r;
    DBT key, val;
    int ret;

    memset(&k, 0, sizeof k);
    memcpy(k.branch, a->branch, sizeof k.branch);
    memcpy(k.user_code, a->user_code, sizeof k.user_code);
    memset(&key, 0, sizeof key);
    memset(&val, 0, sizeof val);
    key.data = &k; key.size = sizeof k;
    val.data = &r; val.ulen = sizeof r; val.flags = DB_DBT_USERMEM;

    ret = a->dbp->get(a->dbp, txn, &key, &val, DB_RMW);
    if (ret) return ret;
    r.failed_attempts = 0;
    val.size = sizeof r;
    return a->dbp->put(a->dbp, txn, &key, &val, 0);
}

int main(void)
{
    DB_ENV *env = NULL;
    DB *dbp = NULL, *sdbp = NULL;
    DBC *cur = NULL;
    DB_TXN *txn = NULL;
    int ret = 0, inserted = 0, existed = 0, scanned = 0, unlocked = 0, by_branch = 0;

    if (mkdir("bdb_env", 0755) != 0 && errno != EEXIST) { perror("mkdir bdb_env"); return 1; }

    /* ---- 1. environment ---- */
    CHECK(db_env_create(&env, 0), "db_env_create");
    env->set_errfile(env, stderr);
    env->set_errpfx(env, "bdbdemo");
    CHECK(env->set_cachesize(env, 0, 16 * 1024 * 1024, 1), "set_cachesize");   /* 16 MB mpool */
    CHECK(env->set_lk_detect(env, DB_LOCK_DEFAULT), "set_lk_detect");          /* detect deadlocks on each blocked request */
    CHECK(env->open(env, "./bdb_env",
                    DB_CREATE | DB_INIT_MPOOL | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN | DB_RECOVER | DB_THREAD,
                    0), "env->open");                                             /* DB_RECOVER: always run recovery at startup */

    /* ---- 2. primary and 3. secondary, two named databases in one file ---- */
    CHECK(db_create(&dbp, env, 0), "db_create");
    CHECK(dbp->open(dbp, NULL, "users.db", "users", DB_BTREE, DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0644), "db->open");

    CHECK(db_create(&sdbp, env, 0), "db_create secondary");
    CHECK(sdbp->set_flags(sdbp, DB_DUPSORT), "set_flags DUPSORT");              /* many users per branch */
    CHECK(sdbp->open(sdbp, NULL, "users.db", "users_by_branch", DB_BTREE, DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0644), "sdb->open");
    /* NULL txn in a transactional env: the associate is auto-committed. DB_CREATE builds the
     * secondary from existing primary records if it is empty. */
    CHECK(dbp->associate(dbp, NULL, sdbp, branch_key_extractor, DB_CREATE), "associate");

    /* ---- 4. insert in one transaction ---- */
    CHECK(env->txn_begin(env, NULL, &txn, 0), "txn_begin");
    for (int i = 0; i < N_USERS; ++i) {
        struct user_key k;
        struct user_rec r;
        DBT key, val;
        fill(&k, &r, i);
        memset(&key, 0, sizeof key);
        memset(&val, 0, sizeof val);
        key.data = &k; key.size = sizeof k;
        val.data = &r; val.size = sizeof r;
        ret = dbp->put(dbp, txn, &key, &val, DB_NOOVERWRITE);                    /* DB_KEYEXIST if present */
        if (ret == DB_KEYEXIST) { ++existed; continue; }
        CHECK(ret, "put");
        ++inserted;
    }
    CHECK(txn->commit(txn, 0), "commit");                                        /* 0 = flush the log: durable */
    txn = NULL;
    printf("insert: %d new, %d already present (DB_KEYEXIST)\n", inserted, existed);

    /* ---- 5. cursor range scan over branch BR0007, unlocking locked users ---- */
    CHECK(env->txn_begin(env, NULL, &txn, 0), "txn_begin");
    CHECK(dbp->cursor(dbp, txn, &cur, 0), "cursor");
    {
        struct user_key start;
        struct user_rec buf;
        DBT key, val;
        memset(&start, 0, sizeof start);              /* user_code all NUL: sorts before any real user */
        memcpy(start.branch, "BR0007", 6);
        memset(&key, 0, sizeof key);
        memset(&val, 0, sizeof val);
        key.data = &start; key.size = sizeof start; key.ulen = sizeof start; key.flags = DB_DBT_USERMEM;
        val.data = &buf;   val.ulen = sizeof buf;     val.flags = DB_DBT_USERMEM;  /* no malloc per record */

        for (ret = cur->get(cur, &key, &val, DB_SET_RANGE);                     /* first key >= start */
             ret == 0;
             ret = cur->get(cur, &key, &val, DB_NEXT)) {
            const struct user_key *k = key.data;
            struct user_rec r;
            if (memcmp(k->branch, "BR0007", 6) != 0) break;                     /* left the branch: stop */
            ++scanned;
            memcpy(&r, val.data, sizeof r);                                     /* copy out, then modify */
            if (r.status == STATUS_LOCKED) {
                DBT upd;
                r.status = STATUS_ACTIVE;
                r.failed_attempts = 0;
                memset(&upd, 0, sizeof upd);
                upd.data = &r; upd.size = sizeof r;
                CHECK(cur->put(cur, &key, &upd, DB_CURRENT), "cursor put");     /* update the record under the cursor */
                ++unlocked;
            }
        }
        if (ret != DB_NOTFOUND && ret != 0) CHECK(ret, "cursor get");
        ret = 0;
    }
    CHECK(cur->close(cur), "cursor close");                                     /* close cursors BEFORE commit */
    cur = NULL;
    CHECK(txn->commit(txn, 0), "commit");
    txn = NULL;
    printf("cursor scan BR0007: %d users, %d unlocked\n", scanned, unlocked);

    /* ---- 6. secondary lookup: every user of BR0011 via the branch index ---- */
    CHECK(env->txn_begin(env, NULL, &txn, 0), "txn_begin");
    CHECK(sdbp->cursor(sdbp, txn, &cur, 0), "secondary cursor");
    {
        char branch[6];
        DBT skey, pkey, pdata;
        memcpy(branch, "BR0011", 6);
        memset(&skey, 0, sizeof skey);
        memset(&pkey, 0, sizeof pkey);
        memset(&pdata, 0, sizeof pdata);
        skey.data = branch; skey.size = sizeof branch;
        /* pkey and pdata left as default: the library returns pointers into its own memory,
         * valid until the next call on this cursor. */
        for (ret = cur->pget(cur, &skey, &pkey, &pdata, DB_SET);
             ret == 0;
             ret = cur->pget(cur, &skey, &pkey, &pdata, DB_NEXT_DUP))
            ++by_branch;
        if (ret != DB_NOTFOUND) CHECK(ret, "pget");
        ret = 0;
    }
    CHECK(cur->close(cur), "cursor close");
    cur = NULL;
    CHECK(txn->commit(txn, 0), "commit");
    txn = NULL;
    printf("secondary BR0011: %d users\n", by_branch);

    /* ---- 7. a unit of work through the deadlock retry wrapper ---- */
    {
        struct reset_arg a = { dbp, "BR0014", "U00000014" };  /* i = 14: branch 14, locked (14 % 7 == 0) */
        CHECK(run_txn(env, reset_attempts_work, &a), "run_txn reset_attempts");
        printf("reset failed_attempts for BR0014/U00000014 under run_txn\n");
    }

    /* ---- 8. checkpoint: in Berkeley DB this is the application's job ---- */
    CHECK(env->txn_checkpoint(env, 0, 0, 0), "checkpoint");
    printf("checkpoint written; now try: db_stat -h bdb_env -c\n");

fail:
    /* close order: cursors, transactions, secondary, primary, environment */
    if (cur) cur->close(cur);
    if (txn) txn->abort(txn);
    if (sdbp) sdbp->close(sdbp, 0);
    if (dbp) dbp->close(dbp, 0);
    if (env) env->close(env, 0);
    return ret == 0 ? 0 : 1;
}
