/* ============================================================================
 * LiteQuery — litequery.h
 * Public C API for the embeddable columnar SQL query engine.
 *
 * This is the ONLY header an application needs. It is pure C89-compatible C so
 * it can be consumed from C, C++, and any language with a C FFI (Python, Rust,
 * Go, …). No C++ types, exceptions, or symbols cross this boundary.
 *
 * Lifecycle
 * ─────────
 *   lq_db*      db  = lq_open();
 *   lq_result*  res = lq_query(db, "SELECT 1+1");
 *   if (lq_result_ok(res)) {
 *       while (lq_result_next(res)) {
 *           int64_t v; lq_result_get_int(res, 0, &v);
 *       }
 *   }
 *   lq_result_free(res);
 *   lq_close(db);
 *
 * Ownership
 * ─────────
 *   - lq_open() returns a db handle you must lq_close().
 *   - lq_query() returns a result handle you must lq_result_free() (even on
 *     error — check lq_result_ok()).
 *   - All returned `const char*` point into engine-owned memory valid until the
 *     next call on the same handle (or until the handle is freed). Copy if you
 *     need to retain them.
 *
 * Thread-safety
 * ─────────────
 *   A single lq_db handle is not thread-safe; serialize access or use one
 *   handle per thread. Distinct handles are independent.
 * ==========================================================================*/

#ifndef LITEQUERY_H
#define LITEQUERY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version -------------------------------------------------------------*/

#define LITEQUERY_VERSION_MAJOR 0
#define LITEQUERY_VERSION_MINOR 1
#define LITEQUERY_VERSION_PATCH 0
#define LITEQUERY_VERSION_STRING "0.1.0"

/* Returns the library version string, e.g. "0.1.0". */
const char* lq_version(void);

/* ---- Opaque handles ------------------------------------------------------*/

typedef struct lq_db     lq_db;      /* A database (a Connection + Catalog).  */
typedef struct lq_result lq_result;  /* The materialized result of a query.   */

/* ---- Error codes ---------------------------------------------------------*/

typedef enum lq_status {
    LQ_OK          = 0,
    LQ_ERROR       = 1,   /* Generic error (see lq_result_error()).           */
    LQ_MISUSE      = 2,   /* API used incorrectly (NULL handle, bad index).   */
    LQ_RANGE       = 3    /* Column/row index out of range.                   */
} lq_status;

/* ---- Column value types (mirror lq::TypeId at the C boundary) ------------*/

typedef enum lq_type {
    LQ_TYPE_NULL    = 0,
    LQ_TYPE_BOOL    = 1,
    LQ_TYPE_INT     = 2,   /* 64-bit signed integer.                          */
    LQ_TYPE_DOUBLE  = 3,   /* 64-bit IEEE float.                              */
    LQ_TYPE_TEXT    = 4    /* UTF-8 string.                                   */
} lq_type;

/* ==========================================================================
 * Database lifecycle
 * ========================================================================*/

/* Open a new in-memory database. Returns NULL only on allocation failure. */
lq_db* lq_open(void);

/* Close a database and free all its tables. Safe to call with NULL. */
void lq_close(lq_db* db);

/* ==========================================================================
 * Query execution
 * ========================================================================*/

/* Execute one SQL statement. Always returns a non-NULL result handle (unless
 * allocation fails); check lq_result_ok(). The handle must be lq_result_free()d.
 */
lq_result* lq_query(lq_db* db, const char* sql);

/* Convenience for statements with no result set (CREATE/INSERT/DROP). Returns
 * LQ_OK on success; writes rows-affected to *affected if non-NULL. On error the
 * message can be retrieved via out_error (engine-owned, valid until next call).
 */
lq_status lq_exec(lq_db* db, const char* sql,
                  int64_t* affected, const char** out_error);

/* ==========================================================================
 * Result inspection
 * ========================================================================*/

/* Non-zero if the query succeeded. */
int lq_result_ok(const lq_result* res);

/* Error message for a failed query (empty string on success). Engine-owned. */
const char* lq_result_error(const lq_result* res);

/* Number of columns / rows in the result set. */
size_t lq_result_column_count(const lq_result* res);
size_t lq_result_row_count(const lq_result* res);

/* Rows affected by a DML/DDL statement. */
int64_t lq_result_rows_affected(const lq_result* res);

/* Elapsed execution time in microseconds. */
int64_t lq_result_elapsed_micros(const lq_result* res);

/* Column name (engine-owned, valid for the life of the result). */
const char* lq_result_column_name(const lq_result* res, size_t col);

/* ==========================================================================
 * Row iteration
 *
 * A freshly returned result is positioned *before* the first row. Call
 * lq_result_next() to advance; it returns non-zero while a row is available.
 * Column accessors operate on the current row.
 * ========================================================================*/

/* Advance to the next row. Returns non-zero if a row is now current, 0 at end. */
int lq_result_next(lq_result* res);

/* Rewind to before the first row. */
void lq_result_reset(lq_result* res);

/* Type of the value at `col` in the current row. */
lq_type lq_result_column_type(const lq_result* res, size_t col);

/* Non-zero if the value at `col` in the current row is NULL. */
int lq_result_is_null(const lq_result* res, size_t col);

/* Typed getters for the current row. Each returns LQ_OK on success and writes
 * *out; LQ_RANGE if col is out of range; LQ_ERROR if there is no current row.
 * A NULL value yields *out = 0 / "" and still returns LQ_OK — use
 * lq_result_is_null() to distinguish. lq_result_get_text() returns a pointer
 * into engine-owned memory valid until the next lq_result_next()/free.
 */
lq_status lq_result_get_int   (const lq_result* res, size_t col, int64_t* out);
lq_status lq_result_get_double(const lq_result* res, size_t col, double*  out);
lq_status lq_result_get_bool  (const lq_result* res, size_t col, int*     out);
const char* lq_result_get_text(const lq_result* res, size_t col);

/* Free a result handle. Safe to call with NULL. */
void lq_result_free(lq_result* res);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LITEQUERY_H */
