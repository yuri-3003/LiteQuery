# litequery — Rust bindings

Safe Rust bindings for [LiteQuery](https://github.com/yuri-3003/LiteQuery), an
embeddable **columnar SQL query engine** that runs analytical queries 3–7×
faster than SQLite. The entire engine is compiled and linked by this crate's
build script — there is **nothing to install separately**.

```rust
use litequery::Connection;

fn main() -> Result<(), litequery::Error> {
    let db = Connection::open()?;
    db.execute("CREATE TABLE t (id INT, name TEXT, amt DOUBLE)")?;
    db.execute("INSERT INTO t VALUES (1,'a',10.5),(2,'b',20),(3,'a',5)")?;

    let result = db.query(
        "SELECT name, SUM(amt) AS total FROM t GROUP BY name ORDER BY name",
    )?;
    for row in result.rows() {
        println!("{} {:?}", row.get_str("name").unwrap(), row.get_f64("total"));
    }

    // Load a CSV straight into a table (names + types inferred):
    db.import_csv("sales.csv", "sales")?;
    println!("{:?}", db.query("SELECT COUNT(*) FROM sales")?.scalar());
    Ok(())
}
```

## Building

The build script uses the [`cc`](https://crates.io/crates/cc) crate to compile
LiteQuery's C++ engine, so you need a **C++17 compiler** on `PATH` (GCC, Clang,
or MSVC). On Windows with the GNU toolchain, use a matching mingw-w64 `g++`.

```bash
cd bindings/rust
cargo build
cargo test
cargo run --example demo
```

That's it — no CMake, no prebuilt library. The engine sources are compiled into
the crate.

## API

### `Connection`

| Method | Description |
|---|---|
| `Connection::open() -> Result<Connection>` | open a new in-memory database |
| `query(sql) -> Result<QueryResult>` | run a `SELECT` |
| `execute(sql) -> Result<i64>` | run `CREATE`/`INSERT`/`DROP`; returns rows affected |
| `import_csv(path, table) -> Result<i64>` | load a CSV into a new table |
| `import_csv_opts(path, table, delimiter, has_header)` | CSV/TSV with options |
| `save(path) -> Result<()>` | save the whole database to a file |
| `load(path) -> Result<()>` | load a database from a file |

The connection closes automatically when dropped.

### `QueryResult`

| Member | Description |
|---|---|
| `columns: Vec<String>` | column names |
| `len()` / `is_empty()` | row count |
| `rows()` | iterator of `Row` views |
| `scalar()` | first column of the first row |
| `rows_affected`, `elapsed_micros` | query metadata |

### `Row`

`row.get("name")` / `row.at(i)` return `Option<&Value>`; the typed helpers
`get_str` / `get_i64` / `get_f64` return `Option<&str>` / `Option<i64>` /
`Option<f64>`.

### `Value`

```rust
enum Value { Null, Bool(bool), Int(i64), Double(f64), Text(String) }
```

with `as_i64()`, `as_f64()`, `as_str()`, and `is_null()`.

### `Error`

Returned (as `Err`) for query, parse, and runtime failures; carries the engine's
message.

## License

MIT.
