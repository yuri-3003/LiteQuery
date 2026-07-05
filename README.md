# LiteQuery

[![CI](https://github.com/yuri-3003/LiteQuery/actions/workflows/ci.yml/badge.svg)](https://github.com/yuri-3003/LiteQuery/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Standard](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](https://en.cppreference.com/w/cpp/17)

An embeddable, zero-dependency columnar SQL query engine in C++17.

LiteQuery drops into any application through a single C header, like SQLite, but
stores data column-by-column and executes with a vectorized pull-based operator
pipeline, so `GROUP BY`, aggregations, and joins run several times faster than a
row store on analytical queries.

```c
#include "litequery/litequery.h"

lq_db* db = lq_open();
lq_exec(db, "CREATE TABLE sales (region TEXT, amount DOUBLE)", NULL, NULL);
lq_exec(db, "INSERT INTO sales VALUES ('West', 100), ('East', 250)", NULL, NULL);

lq_result* r = lq_query(db, "SELECT region, SUM(amount) FROM sales GROUP BY region");
while (lq_result_next(r)) {
    double total; lq_result_get_double(r, 1, &total);
    printf("%s: %.2f\n", lq_result_get_text(r, 0), total);
}
lq_result_free(r);
lq_close(db);
```

- **Zero dependencies.** Just a C++17 compiler. No Boost, no external SQL parser, no build-time codegen.
- **Embeddable.** One public C header (`litequery.h`) and one static library. Usable from C, C++, Python, and Rust.
- **Columnar storage.** Each column is a typed contiguous buffer, so scans and aggregations stay in cache.
- **Vectorized execution.** A typed fast path runs common aggregates over raw arrays with no per-value boxing.

## Try it in 60 seconds

```bash
git clone https://github.com/yuri-3003/LiteQuery.git
cd LiteQuery

# Linux / macOS
./build.sh && ./build/lq_demo

# Windows (PowerShell)
.\build.ps1 ; .\build\lq_demo.exe
```

`build.sh` / `build.ps1` configure, build, run the tests, and leave you a
`liblitequery.a` plus the `lq_demo` and `lq` binaries.

## The `lq` shell

The build produces an interactive shell:

```
$ ./build/lq
LiteQuery 0.2.0  ·  type .help for commands, .quit to exit
lq> CREATE TABLE emp (id INT, name VARCHAR, dept VARCHAR, salary DOUBLE);
OK
lq> INSERT INTO emp VALUES (1,'Ann','Eng',100),(2,'Bob','Eng',120),(3,'Cy','Sales',90);
OK  (3 rows affected)
lq> SELECT dept, COUNT(*), SUM(salary), AVG(salary) FROM emp GROUP BY dept ORDER BY dept;
┌───────┬───────┬─────┬─────┐
│ dept  │ COUNT │ SUM │ AVG │
├───────┼───────┼─────┼─────┤
│ Eng   │ 2     │ 220 │ 110 │
│ Sales │ 1     │ 90  │ 90  │
└───────┴───────┴─────┴─────┘
2 rows  ·  54 µs
```

It runs files (`lq script.sql`), stdin (`echo "SELECT 1" | lq`), and one-shot
statements (`lq -c "SELECT 1+1"`); loads CSV with `.import data.csv t`; has
`.tables` / `.schema` / `.read` meta-commands; and outputs `table`, `csv`, or
`json` via `.mode`. Full guide: [docs/shell.md](docs/shell.md).

Load a CSV and query it directly:

```
lq> .import sales.csv sales
OK  (10432 rows into sales)
lq> SELECT region, SUM(amount) AS total FROM sales GROUP BY region ORDER BY total DESC LIMIT 5;
```

Save a database and reopen it later:

```
lq> .save mydata.lqdb
lq> .open mydata.lqdb
```

The on-disk format serializes the typed columns directly (data plus a validity
bitmap), so all types and NULLs are preserved.

## What it supports

| Area | Support |
|---|---|
| DDL / DML | `CREATE TABLE`, `DROP TABLE`, `INSERT ... VALUES`, `INSERT ... SELECT`, `UPDATE`, `DELETE` |
| Queries | `SELECT` with projections, expressions, aliases; `WHERE`; `GROUP BY`; `HAVING`; `ORDER BY`; `LIMIT` / `OFFSET`; `DISTINCT` |
| Aggregates | `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, `COUNT(DISTINCT ...)`; expressions over aggregates; `HAVING` and `ORDER BY` over aggregates |
| Joins | `INNER`, `LEFT`, `RIGHT`, `FULL`, `CROSS`, with `ON` predicates |
| Set ops | `UNION`, `UNION ALL` |
| Expressions | arithmetic, comparisons, `AND`/`OR`/`NOT`, `\|\|`, `LIKE`/`ILIKE`, `IN`, `BETWEEN`, `CASE`, `CAST`; three-valued NULL logic |
| Functions | `COALESCE`, `ABS`, `LENGTH`, `UPPER`, `LOWER`, `ROUND` |
| Data | CSV/TSV import with type inference; save/load a database file |
| Interfaces | C API, C++ API, `lq` shell, Python and Rust bindings, `EXPLAIN` |

Not yet supported: subqueries in `WHERE` (scalar / `IN (SELECT ...)`),
`INTERSECT` / `EXCEPT`, and Parquet. The full grammar is in
[docs/sql-reference.md](docs/sql-reference.md).

## Performance

Columns are stored in typed contiguous buffers (an `int64`/`double` array plus a
1-bit-per-row validity bitmap). The common analytical query shape runs through a
typed vectorized path with no boxing or per-row `std::variant` dispatch.

### vs SQLite

Same data and queries through LiteQuery and SQLite (3.46.1, in-memory), at 5 M
rows, single thread, `-O2`. Every LiteQuery result is checked equal to SQLite's.

| Query | LiteQuery | SQLite | Speedup |
|---|---|---|---|
| `SELECT SUM(x) FROM t` | 0.021 s | 0.118 s | 5.6x |
| `SELECT AVG(x) FROM t WHERE x>500` | 0.054 s | 0.181 s | 3.3x |
| `SELECT cat, SUM(x) FROM t GROUP BY cat` | 0.220 s | 1.395 s | 6.3x |
| filtered `GROUP BY` | 0.200 s | 1.293 s | 6.5x |
| `SELECT COUNT(*) FROM t` | 0.017 s | 0.001 s | SQLite wins |

LiteQuery is 3-7x faster on analytical aggregations. SQLite wins `COUNT(*)`,
which it answers from metadata rather than scanning. Reproduce:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLITEQUERY_BUILD_SQLITE_BENCH=ON
cmake --build build --target lq_vs_sqlite && ./build/lq_vs_sqlite
```

Numbers and the harness are in [bench/README.md](bench/README.md).

## Build

Requires CMake >= 3.16 and a C++17 compiler (GCC, Clang, or MSVC).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

This produces:

- `liblitequery.a` — the static library to link against.
- `lq` — the interactive shell.
- `lq_demo` — a small CLI tour of the API.
- `lq_tests` / `lq_capi_test` — the C++ and pure-C test suites (ctest).

### Build options

| Option | Default | Effect |
|---|---|---|
| `LITEQUERY_BUILD_TESTS` | `ON`  | Build the test suites |
| `LITEQUERY_BUILD_DEMO`  | `ON`  | Build `lq_demo` |
| `LITEQUERY_BUILD_SHELL` | `ON`  | Build the `lq` shell |
| `LITEQUERY_BUILD_SHARED`| `ON`  | Build the shared library (for FFI / bindings) |
| `LITEQUERY_BUILD_SQLITE_BENCH` | `OFF` | Build the LiteQuery-vs-SQLite benchmark |
| `LITEQUERY_ASAN`        | `OFF` | Build with AddressSanitizer + UBSan |

### Install and consume via CMake

```bash
cmake --install build --prefix /your/install/prefix
```

Then in your own `CMakeLists.txt`:

```cmake
find_package(LiteQuery REQUIRED)
target_link_libraries(myapp PRIVATE LiteQuery::litequery)
```

If your application's own source is `.c` (using the C API), still enable the C++
language in your project (`project(myapp LANGUAGES C CXX)`) because the final
link needs the C++ runtime.

## Using the library

### From C

Include `litequery/litequery.h` and link `liblitequery.a`. See
[docs/c-api.md](docs/c-api.md) for the full reference and the quickstart above.

### From C++

```cpp
#include "connection.h"      // lq::Connection, lq::QueryResult
using namespace lq;

Connection db;
db.query("CREATE TABLE t (id INT, v DOUBLE)");
db.query("INSERT INTO t VALUES (1, 3.5), (2, 7.0)");

QueryResult r = db.query("SELECT id, v FROM t WHERE v > 4 ORDER BY v DESC");
for (const auto& row : r.rows)
    std::cout << row[0].toString() << " = " << row[1].toString() << "\n";

std::cout << db.explain("SELECT SUM(v) FROM t WHERE v > 1 GROUP BY id");
```

### From Python

Pure-`ctypes` bindings (no pybind11 or Cython), so they work with any CPython:

```python
import litequery

with litequery.connect() as db:
    db.import_csv("sales.csv", "sales")           # names and types inferred
    for row in db.query("SELECT region, SUM(amount) AS total "
                        "FROM sales GROUP BY region ORDER BY total DESC"):
        print(row["region"], row["total"])
```

```bash
cd bindings/python && python build_lib.py && pip install .
```

The build produces a self-contained shared library (statically linked C++
runtime), so the module loads on any CPython. Full guide:
[bindings/python/README.md](bindings/python/README.md).

### From Rust

Safe bindings that compile the engine into the crate, so there is no separate
library to install:

```rust
use litequery::Connection;

let db = Connection::open()?;
db.import_csv("sales.csv", "sales")?;
for row in db.query("SELECT region, SUM(amount) AS total \
                     FROM sales GROUP BY region ORDER BY total DESC")?.rows() {
    println!("{} {:?}", row.get_str("region").unwrap(), row.get_f64("total"));
}
# Ok::<(), litequery::Error>(())
```

```bash
cd bindings/rust && cargo test && cargo run --example demo
```

Full guide: [bindings/rust/README.md](bindings/rust/README.md).

## How it works

```
SQL text -> Lexer -> Parser -> AST -> operator tree -> QueryResult
```

The `Connection` builds a physical operator tree directly from the AST
(`SeqScan`, `Filter`, `Project`, `HashAggregate`, `HashJoin`, `Sort`,
`Distinct`, `Limit`, `Append`). Each operator implements `next()` returning a
columnar `Batch`; the root is pulled in a loop until empty. `EXPLAIN` prints the
same tree the executor runs. A typed fast path handles the common aggregate
shape over raw column arrays.

Full walkthrough: [docs/architecture.md](docs/architecture.md).

## Documentation

- [docs/architecture.md](docs/architecture.md) — the query lifecycle and each component.
- [docs/sql-reference.md](docs/sql-reference.md) — the supported SQL grammar and semantics.
- [docs/c-api.md](docs/c-api.md) — the C API reference, ownership rules, and threading.
- [docs/shell.md](docs/shell.md) — the `lq` shell.

## License

MIT. See [LICENSE](LICENSE).
