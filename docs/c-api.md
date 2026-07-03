# LiteQuery C API Reference

The public API is a single header, `include/litequery/litequery.h`. It is pure
C, so it is callable from C, C++, and any language with a C FFI. No C++ types,
symbols, or exceptions cross this boundary.

Link against `liblitequery.a` (and the C++ standard library, since the
implementation is C++ — most toolchains link it automatically when the final
link is driven by the C++ driver; with a C driver, add `-lstdc++`).

## Lifecycle

```c
#include "litequery/litequery.h"

lq_db* db = lq_open();                       // open an in-memory database
lq_result* r = lq_query(db, "SELECT 1+1");   // run a query
/* ... use r ... */
lq_result_free(r);                           // free the result
lq_close(db);                                // close the database
```

**Ownership rules**

- `lq_open()` → you must `lq_close()`.
- `lq_query()` → you must `lq_result_free()`, **even on error** (check
  `lq_result_ok()` first).
- Every returned `const char*` points into engine-owned memory that is valid
  **until the next call on the same handle** (or until it is freed). Copy the
  string if you need to keep it.

**Thread-safety.** A single `lq_db` is not thread-safe; use one per thread or
serialize access. Distinct handles are independent.

## Handles & enums

```c
typedef struct lq_db     lq_db;      /* a database (connection + catalog) */
typedef struct lq_result lq_result;  /* a materialized query result       */

typedef enum { LQ_OK=0, LQ_ERROR=1, LQ_MISUSE=2, LQ_RANGE=3 } lq_status;

typedef enum {
    LQ_TYPE_NULL=0, LQ_TYPE_BOOL=1, LQ_TYPE_INT=2,
    LQ_TYPE_DOUBLE=3, LQ_TYPE_TEXT=4
} lq_type;
```

## Functions

### Version

```c
const char* lq_version(void);   /* e.g. "0.1.0" */
```

### Database

```c
lq_db* lq_open(void);           /* NULL only on allocation failure */
void   lq_close(lq_db* db);     /* safe on NULL */
```

### Execution

```c
/* Run one statement; always returns a handle to free (check lq_result_ok). */
lq_result* lq_query(lq_db* db, const char* sql);

/* Convenience for non-result statements (CREATE/INSERT/DROP).
 * Returns LQ_OK on success and writes rows-affected to *affected (if non-NULL).
 * On error returns LQ_ERROR and, if out_error is non-NULL, points it at the
 * (thread-local, engine-owned) error message. */
lq_status lq_exec(lq_db* db, const char* sql,
                  int64_t* affected, const char** out_error);
```

### Result metadata

```c
int         lq_result_ok(const lq_result* r);              /* non-zero on success */
const char* lq_result_error(const lq_result* r);           /* "" on success */
size_t      lq_result_column_count(const lq_result* r);
size_t      lq_result_row_count(const lq_result* r);
int64_t     lq_result_rows_affected(const lq_result* r);
int64_t     lq_result_elapsed_micros(const lq_result* r);
const char* lq_result_column_name(const lq_result* r, size_t col);
```

### Row iteration

A fresh result is positioned **before** the first row.

```c
int  lq_result_next(lq_result* r);    /* advance; non-zero while a row is current */
void lq_result_reset(lq_result* r);   /* rewind to before the first row */
```

### Value access (current row)

```c
lq_type     lq_result_column_type(const lq_result* r, size_t col);
int         lq_result_is_null(const lq_result* r, size_t col);

lq_status   lq_result_get_int   (const lq_result* r, size_t col, int64_t* out);
lq_status   lq_result_get_double(const lq_result* r, size_t col, double*  out);
lq_status   lq_result_get_bool  (const lq_result* r, size_t col, int*     out);
const char* lq_result_get_text  (const lq_result* r, size_t col);
```

Getters return `LQ_OK` on success, `LQ_RANGE` if `col` is out of range, and
`LQ_ERROR` if there is no current row. A `NULL` value yields `*out = 0`/`""` and
still returns `LQ_OK` — use `lq_result_is_null()` to distinguish. The pointer
from `lq_result_get_text()` is valid until the next call on that result.

```c
void lq_result_free(lq_result* r);    /* safe on NULL */
```

## Complete example

```c
#include "litequery/litequery.h"
#include <stdio.h>

int main(void) {
    lq_db* db = lq_open();

    lq_exec(db, "CREATE TABLE t (id INT, region TEXT, amt DOUBLE)", NULL, NULL);
    lq_exec(db, "INSERT INTO t VALUES (1,'W',100),(2,'E',250),(3,'W',60)",
            NULL, NULL);

    lq_result* r = lq_query(db,
        "SELECT region, COUNT(*), SUM(amt) FROM t GROUP BY region ORDER BY region");

    if (!lq_result_ok(r)) {
        fprintf(stderr, "error: %s\n", lq_result_error(r));
    } else {
        for (size_t c = 0; c < lq_result_column_count(r); ++c)
            printf("%s\t", lq_result_column_name(r, c));
        printf("\n");
        while (lq_result_next(r)) {
            int64_t n; double s;
            lq_result_get_int(r, 1, &n);
            lq_result_get_double(r, 2, &s);
            printf("%s\t%lld\t%.2f\n", lq_result_get_text(r, 0),
                   (long long)n, s);
        }
    }

    lq_result_free(r);
    lq_close(db);
    return 0;
}
```
