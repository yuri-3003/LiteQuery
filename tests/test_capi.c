/* ============================================================================
 * LiteQuery — test_capi.c
 * A pure-C test that uses ONLY the public C header. Its existence proves the
 * engine is embeddable from plain C with no C++ leakage across the boundary.
 * ==========================================================================*/

#include "litequery/litequery.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (!(cond)) { printf("  FAIL: %s\n", msg); ++failures; } \
        else         { printf("  ok:   %s\n", msg); }           \
    } while (0)

int main(void) {
    printf("LiteQuery C API test (v%s)\n", lq_version());

    lq_db* db = lq_open();
    CHECK(db != NULL, "lq_open returns a handle");

    int64_t aff = -1;
    const char* err = NULL;

    CHECK(lq_exec(db, "CREATE TABLE t (id INT, name VARCHAR, amt DOUBLE)",
                  &aff, &err) == LQ_OK, "CREATE TABLE");
    CHECK(lq_exec(db, "INSERT INTO t VALUES (1,'a',10.5),(2,'b',20.0),(3,'a',5.0)",
                  &aff, &err) == LQ_OK, "INSERT");
    CHECK(aff == 3, "INSERT affected 3 rows");

    /* Aggregate + GROUP BY + ORDER BY over the C boundary. */
    lq_result* r = lq_query(db, "SELECT name, SUM(amt) FROM t GROUP BY name ORDER BY name");
    CHECK(lq_result_ok(r), "GROUP BY query ok");
    CHECK(lq_result_column_count(r) == 2, "2 columns");
    CHECK(lq_result_row_count(r) == 2, "2 groups");
    CHECK(strcmp(lq_result_column_name(r, 0), "name") == 0, "column 0 is 'name'");

    int row = 0;
    while (lq_result_next(r)) {
        const char* nm = lq_result_get_text(r, 0);
        double s = 0.0;
        lq_result_get_double(r, 1, &s);
        if (row == 0) CHECK(strcmp(nm, "a") == 0 && s == 15.5, "group a sums to 15.5");
        if (row == 1) CHECK(strcmp(nm, "b") == 0 && s == 20.0, "group b sums to 20.0");
        ++row;
    }
    lq_result_free(r);

    /* NULL handling via LEFT JOIN. */
    lq_exec(db, "CREATE TABLE d (id INT, dn VARCHAR)", &aff, &err);
    lq_exec(db, "INSERT INTO d VALUES (1,'X')", &aff, &err);
    lq_exec(db, "CREATE TABLE e (id INT, did INT)", &aff, &err);
    lq_exec(db, "INSERT INTO e VALUES (1,1),(2,9)", &aff, &err);
    lq_result* j = lq_query(db,
        "SELECT e.id, d.dn FROM e AS e LEFT JOIN d AS d ON e.did = d.id ORDER BY e.id");
    CHECK(lq_result_ok(j), "LEFT JOIN ok");
    CHECK(lq_result_row_count(j) == 2, "2 result rows");
    lq_result_next(j);                       /* row 0: id=1, dn='X' */
    lq_result_next(j);                       /* row 1: id=2, dn=NULL */
    CHECK(lq_result_is_null(j, 1) == 1, "unmatched LEFT JOIN yields NULL");
    lq_result_free(j);

    /* Error path: querying an unknown table must not crash. */
    lq_result* bad = lq_query(db, "SELECT * FROM nope");
    CHECK(lq_result_ok(bad) == 0, "unknown table flagged as error");
    CHECK(strlen(lq_result_error(bad)) > 0, "error message is present");
    lq_result_free(bad);

    lq_close(db);

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL C API TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
