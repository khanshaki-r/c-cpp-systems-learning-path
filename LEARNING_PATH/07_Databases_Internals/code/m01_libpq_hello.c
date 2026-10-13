/* m01_libpq_hello.c : talk to PostgreSQL at the protocol level with libpq (module 01).
 *
 * Build and run on WSL Ubuntu 24.04:
 *     sudo apt install -y libpq-dev
 *     gcc -Wall -Wextra m01_libpq_hello.c -o hello -I/usr/include/postgresql -lpq
 *     ./hello                         (connects with "dbname=loans")
 *     ./hello "host=localhost port=5433 user=yugabyte dbname=yugabyte"   (same code against YSQL)
 *
 * What it shows:
 *   - PQconnectdb and the connection status check
 *   - PQexecParams: the extended protocol (Parse/Bind/Execute), parameters sent separately from
 *     the SQL text, so there is no SQL injection and no quoting
 *   - reading the result set and NULLs
 *   - SQLSTATE from an error, the machine-readable field your C server should switch on
 *   - the plan the server chose for the same parameterised statement
 */
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(PGconn *conn, const char *what)
{
    fprintf(stderr, "%s: %s", what, PQerrorMessage(conn));
    PQfinish(conn);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *conninfo = argc > 1 ? argv[1] : "dbname=loans";
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) die(conn, "connect");

    printf("server version %d, backend pid %d\n", PQserverVersion(conn), PQbackendPID(conn));

    /* 1. Extended protocol: Parse/Bind/Execute. The server plans with the real parameter value,
     *    so it can use the statistics for that value (a custom plan). */
    const char *params[1] = { "LA-2024-000123" };
    PGresult *res = PQexecParams(conn,
        "SELECT customer_id, status, submitted_at FROM loan_applications WHERE application_no = $1",
        1,          /* one parameter */
        NULL,       /* let the server infer its type from the column: text */
        params,
        NULL,       /* text-format parameters need no lengths */
        NULL,       /* all text format */
        0);         /* text results */
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "query failed: %s", PQerrorMessage(conn));    /* server-side error text */
        PQclear(res);
        PQfinish(conn);
        return 1;
    }
    for (int row = 0; row < PQntuples(res); ++row) {
        printf("customer_id=%s status=%s submitted_at=%s\n",
               PQgetvalue(res, row, 0),
               PQgetvalue(res, row, 1),
               PQgetisnull(res, row, 2) ? "NULL" : PQgetvalue(res, row, 2));   /* NULL is not "" */
    }
    PQclear(res);

    /* 2. The same statement under EXPLAIN: what the planner did with it. */
    res = PQexecParams(conn,
        "EXPLAIN (ANALYZE, BUFFERS) SELECT customer_id, status FROM loan_applications WHERE application_no = $1",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        printf("\nplan:\n");
        for (int row = 0; row < PQntuples(res); ++row) printf("  %s\n", PQgetvalue(res, row, 0));
    }
    PQclear(res);

    /* 3. An error, read the way a server should read it: by SQLSTATE, not by message text. */
    res = PQexec(conn, "SELECT * FROM no_such_table");
    if (PQresultStatus(res) == PGRES_FATAL_ERROR) {
        const char *state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        printf("\nexpected error: SQLSTATE %s (%s)\n", state ? state : "?",
               state && strcmp(state, "42P01") == 0 ? "undefined_table" : "other");
    }
    PQclear(res);

    PQfinish(conn);       /* sends Terminate; the backend process exits */
    return 0;
}
