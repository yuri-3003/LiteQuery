//! Raw FFI declarations for the LiteQuery C API (see `include/litequery/litequery.h`).
//!
//! Everything here is `unsafe`; the safe wrapper lives in `lib.rs`. Some items
//! mirror the full C API for completeness even if the safe layer doesn't use
//! them yet.
#![allow(dead_code)]

use std::os::raw::{c_char, c_double, c_int, c_void};

pub type LqDb = c_void;
pub type LqResult = c_void;

// lq_status
pub const LQ_OK: c_int = 0;

// lq_type
pub const LQ_TYPE_NULL: c_int = 0;
pub const LQ_TYPE_BOOL: c_int = 1;
pub const LQ_TYPE_INT: c_int = 2;
pub const LQ_TYPE_DOUBLE: c_int = 3;
pub const LQ_TYPE_TEXT: c_int = 4;

extern "C" {
    pub fn lq_version() -> *const c_char;

    pub fn lq_open() -> *mut LqDb;
    pub fn lq_close(db: *mut LqDb);

    pub fn lq_query(db: *mut LqDb, sql: *const c_char) -> *mut LqResult;

    pub fn lq_import_csv(
        db: *mut LqDb,
        path: *const c_char,
        table_name: *const c_char,
        delimiter: c_char,
        has_header: c_int,
        rows: *mut i64,
        out_error: *mut *const c_char,
    ) -> c_int;

    pub fn lq_save(db: *mut LqDb, path: *const c_char, out_error: *mut *const c_char) -> c_int;
    pub fn lq_load(db: *mut LqDb, path: *const c_char, out_error: *mut *const c_char) -> c_int;

    pub fn lq_result_ok(res: *const LqResult) -> c_int;
    pub fn lq_result_error(res: *const LqResult) -> *const c_char;
    pub fn lq_result_column_count(res: *const LqResult) -> usize;
    pub fn lq_result_row_count(res: *const LqResult) -> usize;
    pub fn lq_result_rows_affected(res: *const LqResult) -> i64;
    pub fn lq_result_elapsed_micros(res: *const LqResult) -> i64;
    pub fn lq_result_column_name(res: *const LqResult, col: usize) -> *const c_char;

    pub fn lq_result_next(res: *mut LqResult) -> c_int;
    pub fn lq_result_reset(res: *mut LqResult);
    pub fn lq_result_column_type(res: *const LqResult, col: usize) -> c_int;
    pub fn lq_result_is_null(res: *const LqResult, col: usize) -> c_int;

    pub fn lq_result_get_int(res: *const LqResult, col: usize, out: *mut i64) -> c_int;
    pub fn lq_result_get_double(res: *const LqResult, col: usize, out: *mut c_double) -> c_int;
    pub fn lq_result_get_bool(res: *const LqResult, col: usize, out: *mut c_int) -> c_int;
    pub fn lq_result_get_text(res: *const LqResult, col: usize) -> *const c_char;

    pub fn lq_result_free(res: *mut LqResult);
}
