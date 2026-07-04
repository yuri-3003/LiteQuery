//! # LiteQuery — Rust bindings
//!
//! Safe Rust bindings for [LiteQuery](https://github.com/yuri-3003/LiteQuery),
//! an embeddable **columnar SQL query engine** that runs analytical queries
//! (GROUP BY, aggregations, joins) 3–7× faster than SQLite. The whole engine is
//! compiled and linked by this crate's build script — there is nothing to
//! install separately.
//!
//! ```no_run
//! use litequery::Connection;
//!
//! let db = Connection::open()?;
//! db.execute("CREATE TABLE t (id INT, name TEXT, amt DOUBLE)")?;
//! db.execute("INSERT INTO t VALUES (1,'a',10.5),(2,'b',20),(3,'a',5)")?;
//!
//! let result = db.query(
//!     "SELECT name, SUM(amt) AS total FROM t GROUP BY name ORDER BY name",
//! )?;
//! for row in result.rows() {
//!     println!("{} {:?}", row.get_str("name").unwrap(), row.get("total"));
//! }
//! # Ok::<(), litequery::Error>(())
//! ```

use std::ffi::{CStr, CString};
use std::fmt;
use std::os::raw::c_char;
use std::ptr;

mod ffi;

/// An error returned by a query or operation.
#[derive(Debug, Clone)]
pub struct Error(pub String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "LiteQuery error: {}", self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// A single scalar value from a result row.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    Int(i64),
    Double(f64),
    Text(String),
}

impl Value {
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Value::Int(v) => Some(*v),
            Value::Double(v) => Some(*v as i64),
            Value::Bool(b) => Some(*b as i64),
            _ => None,
        }
    }
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Value::Double(v) => Some(*v),
            Value::Int(v) => Some(*v as f64),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Text(s) => Some(s.as_str()),
            _ => None,
        }
    }
    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }
}

/// The LiteQuery library version string, e.g. `"0.2.0"`.
pub fn version() -> String {
    unsafe {
        let p = ffi::lq_version();
        cstr_to_string(p).unwrap_or_default()
    }
}

/// A database connection (in-memory). Closed automatically when dropped.
///
/// Not `Sync`/`Send`-safe to share concurrently; use one connection per thread.
pub struct Connection {
    db: *mut ffi::LqDb,
}

impl Connection {
    /// Open a new in-memory database.
    pub fn open() -> Result<Connection> {
        let db = unsafe { ffi::lq_open() };
        if db.is_null() {
            return Err(Error("failed to open database".into()));
        }
        Ok(Connection { db })
    }

    /// Run a `SELECT` and return the materialized result set.
    pub fn query(&self, sql: &str) -> Result<QueryResult> {
        let c_sql = CString::new(sql).map_err(|_| Error("SQL contains a NUL byte".into()))?;
        let res = unsafe { ffi::lq_query(self.db, c_sql.as_ptr()) };
        if res.is_null() {
            return Err(Error("query returned no result handle".into()));
        }
        // Wrap so the handle is freed on any early return.
        let handle = ResultHandle(res);

        if unsafe { ffi::lq_result_ok(res) } == 0 {
            let msg = unsafe { cstr_to_string(ffi::lq_result_error(res)) }.unwrap_or_default();
            return Err(Error(msg));
        }

        let ncol = unsafe { ffi::lq_result_column_count(res) };
        let mut columns = Vec::with_capacity(ncol);
        for c in 0..ncol {
            let name = unsafe { cstr_to_string(ffi::lq_result_column_name(res, c)) }
                .unwrap_or_default();
            columns.push(name);
        }

        let mut rows: Vec<Vec<Value>> = Vec::new();
        while unsafe { ffi::lq_result_next(res) } != 0 {
            let mut row = Vec::with_capacity(ncol);
            for c in 0..ncol {
                row.push(unsafe { extract_value(res, c) });
            }
            rows.push(row);
        }

        let rows_affected = unsafe { ffi::lq_result_rows_affected(res) };
        let elapsed_micros = unsafe { ffi::lq_result_elapsed_micros(res) };
        drop(handle); // frees the C result

        Ok(QueryResult {
            columns,
            rows,
            rows_affected,
            elapsed_micros,
        })
    }

    /// Run a statement with no result set (`CREATE`/`INSERT`/`DROP`).
    /// Returns the number of rows affected.
    pub fn execute(&self, sql: &str) -> Result<i64> {
        Ok(self.query(sql)?.rows_affected)
    }

    /// Load a CSV/TSV file into a new table, inferring names and types.
    /// Returns the number of rows loaded.
    pub fn import_csv(&self, path: &str, table: &str) -> Result<i64> {
        self.import_csv_opts(path, table, b',', true)
    }

    /// Like [`import_csv`](Self::import_csv) with an explicit delimiter and
    /// header flag (`b'\t'` for TSV).
    pub fn import_csv_opts(
        &self,
        path: &str,
        table: &str,
        delimiter: u8,
        has_header: bool,
    ) -> Result<i64> {
        let c_path = CString::new(path).map_err(|_| Error("path contains a NUL byte".into()))?;
        let c_table =
            CString::new(table).map_err(|_| Error("table name contains a NUL byte".into()))?;
        let mut rows: i64 = 0;
        let mut err: *const c_char = ptr::null();
        let status = unsafe {
            ffi::lq_import_csv(
                self.db,
                c_path.as_ptr(),
                c_table.as_ptr(),
                delimiter as c_char,
                has_header as i32,
                &mut rows,
                &mut err,
            )
        };
        if status != ffi::LQ_OK {
            let msg = unsafe { cstr_to_string(err) }.unwrap_or_else(|| "CSV import failed".into());
            return Err(Error(msg));
        }
        Ok(rows)
    }

    /// Save the whole database (all tables + data) to a single file.
    pub fn save(&self, path: &str) -> Result<()> {
        self.path_fn(path, ffi::lq_save, "save failed")
    }

    /// Load a database from a file (tables are added, replacing same names).
    pub fn load(&self, path: &str) -> Result<()> {
        self.path_fn(path, ffi::lq_load, "load failed")
    }

    fn path_fn(
        &self,
        path: &str,
        f: unsafe extern "C" fn(*mut ffi::LqDb, *const c_char, *mut *const c_char) -> std::os::raw::c_int,
        default_err: &str,
    ) -> Result<()> {
        let c_path = CString::new(path).map_err(|_| Error("path contains a NUL byte".into()))?;
        let mut err: *const c_char = ptr::null();
        let status = unsafe { f(self.db, c_path.as_ptr(), &mut err) };
        if status != ffi::LQ_OK {
            let msg = unsafe { cstr_to_string(err) }.unwrap_or_else(|| default_err.into());
            return Err(Error(msg));
        }
        Ok(())
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        if !self.db.is_null() {
            unsafe { ffi::lq_close(self.db) };
            self.db = ptr::null_mut();
        }
    }
}

/// A materialized query result: column names and rows of [`Value`]s.
#[derive(Debug, Clone)]
pub struct QueryResult {
    pub columns: Vec<String>,
    rows: Vec<Vec<Value>>,
    pub rows_affected: i64,
    pub elapsed_micros: i64,
}

impl QueryResult {
    /// Number of rows in the result set.
    pub fn len(&self) -> usize {
        self.rows.len()
    }
    pub fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }

    /// Iterate result rows as [`Row`] views (borrow the column names).
    pub fn rows(&self) -> impl Iterator<Item = Row<'_>> {
        self.rows.iter().map(move |values| Row {
            columns: &self.columns,
            values,
        })
    }

    /// The value in the first column of the first row, if any.
    pub fn scalar(&self) -> Option<&Value> {
        self.rows.first().and_then(|r| r.first())
    }
}

/// A borrowed view of one result row: access by column name or index.
pub struct Row<'a> {
    columns: &'a [String],
    values: &'a [Value],
}

impl<'a> Row<'a> {
    /// The value at column index `i`.
    pub fn at(&self, i: usize) -> Option<&Value> {
        self.values.get(i)
    }
    /// The value for a column by name.
    pub fn get(&self, name: &str) -> Option<&Value> {
        let idx = self.columns.iter().position(|c| c == name)?;
        self.values.get(idx)
    }
    /// Convenience: a column's value as `&str` (None if absent or not text).
    pub fn get_str(&self, name: &str) -> Option<&str> {
        self.get(name).and_then(|v| v.as_str())
    }
    /// Convenience: a column's value as `f64`.
    pub fn get_f64(&self, name: &str) -> Option<f64> {
        self.get(name).and_then(|v| v.as_f64())
    }
    /// Convenience: a column's value as `i64`.
    pub fn get_i64(&self, name: &str) -> Option<i64> {
        self.get(name).and_then(|v| v.as_i64())
    }
    /// All values in this row.
    pub fn values(&self) -> &[Value] {
        self.values
    }
}

// ---- internal helpers ------------------------------------------------------

// RAII guard that frees a C result handle when dropped.
struct ResultHandle(*mut ffi::LqResult);
impl Drop for ResultHandle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { ffi::lq_result_free(self.0) };
        }
    }
}

unsafe fn cstr_to_string(p: *const c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    Some(CStr::from_ptr(p).to_string_lossy().into_owned())
}

unsafe fn extract_value(res: *const ffi::LqResult, col: usize) -> Value {
    if ffi::lq_result_is_null(res, col) != 0 {
        return Value::Null;
    }
    match ffi::lq_result_column_type(res, col) {
        ffi::LQ_TYPE_NULL => Value::Null,
        ffi::LQ_TYPE_INT => {
            let mut v: i64 = 0;
            ffi::lq_result_get_int(res, col, &mut v);
            Value::Int(v)
        }
        ffi::LQ_TYPE_DOUBLE => {
            let mut v: f64 = 0.0;
            ffi::lq_result_get_double(res, col, &mut v);
            Value::Double(v)
        }
        ffi::LQ_TYPE_BOOL => {
            let mut v: i32 = 0;
            ffi::lq_result_get_bool(res, col, &mut v);
            Value::Bool(v != 0)
        }
        _ => {
            let p = ffi::lq_result_get_text(res, col);
            Value::Text(cstr_to_string(p).unwrap_or_default())
        }
    }
}
