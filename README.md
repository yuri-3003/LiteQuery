# LiteQuery

[![CI](https://github.com/yuri-3003/LiteQuery/actions/workflows/ci.yml/badge.svg)](https://github.com/yuri-3003/LiteQuery/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Standard](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](https://en.cppreference.com/w/cpp/17)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-brightgreen.svg)](#build)

**An embeddable, zero-dependency columnar SQL query engine in C++17.**

LiteQuery drops into any application through a single C header — exactly like
SQLite — but stores data **column-by-column** and executes with a **vectorized
pull-based operator pipeline**, the architecture analytical databases use to run
`GROUP BY`, aggregations, and joins efficiently.

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
- **Truly embeddable.** One public C header (`litequery.h`); link one static library. Usable from C, C++, and any language with a C FFI.
- **Columnar storage.** Each column is stored contiguously so scans and aggregations are cache-friendly.
- **Real query pipeline.** Lexer → Parser → Logical plan → Rule-based optimizer → Vectorized physical operators.

---

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

---

## The `lq` shell

Prefer typing SQL over writing code? The build produces an interactive shell:

```
$ ./build/lq
LiteQuery v0.1.0  ·  type .help for commands, .quit to exit
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
statements (`lq -c "SELECT 1+1"`); loads real data with `.import data.csv t`;
has `.tables` / `.schema` / `.read` meta-commands; and outputs `table`, `csv`,
or `json` via `.mode`. Full guide: [docs/shell.md](docs/shell.md).

Point it at a real CSV and query it immediately:

```
lq> .import sales.csv sales
OK  (10432 rows into sales)
lq> SELECT region, SUM(amount) AS total FROM sales GROUP BY region ORDER BY total DESC LIMIT 5;
```

---

## Status

LiteQuery is an **early but genuinely working** engine (v0.1). The full path from
SQL text to results is implemented, compiled, and covered by an automated test
suite that runs green under a Release build.

**What works today** (all verified end-to-end):

| Feature | Status |
|---|---|
| `CREATE TABLE` / `DROP TABLE` / `INSERT … VALUES` | ✅ |
| `SELECT` with projections, expressions, aliases | ✅ |
| `WHERE` with full 3-valued boolean logic & NULL semantics | ✅ |
| Arithmetic, comparisons, `AND`/`OR`/`NOT`, `||`, `LIKE`/`ILIKE`, `IN`, `BETWEEN`, `CASE`, `CAST` | ✅ |
| Scalar functions: `COALESCE`, `ABS`, `LENGTH`, `UPPER`, `LOWER`, `ROUND` | ✅ |
| `GROUP BY` with `COUNT`/`SUM`/`AVG`/`MIN`/`MAX` (+ `COUNT(DISTINCT …)`) | ✅ |
| `JOIN` — `INNER`, `LEFT`, `RIGHT`, `FULL`, `CROSS`, with `ON` predicates | ✅ |
| `DISTINCT`, `ORDER BY` (incl. by dropped columns), `LIMIT` / `OFFSET` | ✅ |
| CSV/TSV ingestion with automatic type inference (`.import` / `importCsv`) | ✅ |
| Rule-based optimizer (constant folding, predicate pushdown, column pruning, …) | ✅ |
| `EXPLAIN` (logical plan printer) | ✅ |
| Interactive `lq` shell (REPL, table/csv/json output, `.import`) | ✅ |
| C API (`lq_open`/`lq_query`/`lq_result_*`) — used from pure C | ✅ |

**Known limitations** (candidly): the typed columnar fast path covers the common
aggregate shape (`SELECT [key,] AGG(col)… FROM t [WHERE simple] [GROUP BY key]`);
other queries still use the correct-but-boxed general executor. `HAVING` and
subqueries in `WHERE`/`FROM` are partial; `INSERT … SELECT` is not implemented;
the optimizer runs on a logical plan that is separate from the executor's
AST-driven path. See [docs/architecture.md](docs/architecture.md) and the
[Roadmap](#roadmap).

> Note on provenance: this repository was reconstructed from a partial snapshot.
> The front-end (type system, lexer, AST, logical planner, optimizer) predates
> this work; the parser, storage, evaluator, physical operators, connection
> pipeline, C API, tests, build, and docs were completed to make it run.

---

## Performance

Data is stored **column-by-column** in typed contiguous buffers (an `int64`/
`double` array + a 1-bit-per-row validity bitmap), and the common analytical
query shape runs through a **typed vectorized execution path** — no boxing, no
per-row `std::variant` dispatch.

### vs SQLite

The same data and queries through LiteQuery and SQLite (3.46.1, in-memory), at
5 M rows, single thread, `-O2`. **Every LiteQuery result is verified equal to
SQLite's.**

| Query | LiteQuery | SQLite | Speedup |
|---|---|---|---|
| `SELECT SUM(x) FROM t` | 0.021 s | 0.118 s | **5.6× faster** |
| `SELECT AVG(x) FROM t WHERE x>500` | 0.054 s | 0.181 s | **3.3× faster** |
| `SELECT cat, SUM(x) FROM t GROUP BY cat` | 0.220 s | 1.395 s | **6.3× faster** |
| filtered `GROUP BY` | 0.200 s | 1.293 s | **6.5× faster** |
| `SELECT COUNT(*) FROM t` | 0.017 s | 0.001 s | 0.1× (SQLite wins) |

LiteQuery is **3–7× faster on the analytical aggregations** it's built for.
SQLite wins `COUNT(*)` — it answers from metadata instead of scanning, which
LiteQuery doesn't special-case yet; reporting that honestly is the point.

Reproduce:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLITEQUERY_BUILD_SQLITE_BENCH=ON
cmake --build build --target lq_vs_sqlite && ./build/lq_vs_sqlite
```

Against LiteQuery's own earlier boxed executor the typed path is 3–26× faster;
full numbers and the harness are in [bench/README.md](bench/README.md).

---

## Build

Requires **CMake ≥ 3.16** and a **C++17** compiler (GCC, Clang, or MSVC).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

This produces:

- `liblitequery.a` — the static library to link against.
- `lq_tests` — the C++ unit/integration suite (ctest target `unit`).
- `lq_capi_test` — a pure-C test using only the public header (ctest target `capi`).
- `lq_demo` — a small CLI tour of the API.

Run the demo:

```bash
./build/lq_demo
```

### Build options

| Option | Default | Effect |
|---|---|---|
| `LITEQUERY_BUILD_TESTS` | `ON`  | Build `lq_tests` and `lq_capi_test` |
| `LITEQUERY_BUILD_DEMO`  | `ON`  | Build `lq_demo` |
| `LITEQUERY_ASAN`        | `OFF` | Build with AddressSanitizer + UBSan (where the toolchain provides them) |

### Install & consume via CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /your/install/prefix
```

Then in your own project's `CMakeLists.txt`:

```cmake
find_package(LiteQuery REQUIRED)
target_link_libraries(myapp PRIVATE LiteQuery::litequery)
```

> If your application's own source is `.c` (using the C API), still enable the
> C++ language in your project — `project(myapp LANGUAGES C CXX)` — because the
> final link needs the C++ runtime.

### One-line build (no CMake)

```bash
g++ -std=c++17 -O2 -Iinclude -Iinclude/litequery \
    -Isrc/parser -Isrc/planner -Isrc/catalog -Isrc/storage -Isrc/execution -Isrc/api \
    src/parser/lexer.cpp src/parser/parser.cpp \
    src/planner/logical_plan.cpp src/planner/optimizer.cpp \
    src/execution/eval.cpp src/execution/physical_plan.cpp \
    src/api/connection.cpp src/api/c_api.cpp \
    -c && ar rcs liblitequery.a *.o
```

---

## Using the library

### From C (the embeddable path)

Include one header and link `liblitequery.a`. See
[docs/c-api.md](docs/c-api.md) for the full reference and the
[quickstart above](#litequery).

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

// Inspect the optimized logical plan:
std::cout << db.explain("SELECT SUM(v) FROM t WHERE v > 1 GROUP BY id");
```

### From Python

Pure-`ctypes` bindings — no pybind11/Cython, works with any CPython:

```python
import litequery

with litequery.connect() as db:
    db.import_csv("sales.csv", "sales")           # names + types inferred
    for row in db.query("SELECT region, SUM(amount) AS total "
                        "FROM sales GROUP BY region ORDER BY total DESC"):
        print(row["region"], row["total"])
```

```bash
cd bindings/python && python build_lib.py && pip install .
```

The build produces a **self-contained** shared library (statically linked C++
runtime), so the module loads on any CPython regardless of how it was compiled.
Full guide: [bindings/python/README.md](bindings/python/README.md).

### From Rust

Safe bindings that compile the whole engine into the crate — no CMake, no
prebuilt library:

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

---

## Architecture at a glance

```
   SQL text
      │  Lexer            src/parser/lexer.*      → vector<Token>
      ▼
   tokens
      │  Parser           src/parser/parser.*     → ast::StmtNode
      ▼
   AST  ──────────────┬────────────────────────────────────────────┐
      │               │  LogicalPlanner  src/planner/logical_plan.* │  (EXPLAIN path)
      │               ▼                                             │
      │           LogicalPlan ──► Optimizer  src/planner/optimizer.*│
      │                                                             │
      │  Connection builds a physical operator tree directly from   │
      ▼  the resolved AST + Catalog:                                ▼
   Operator tree   src/execution/physical_plan.*        PlanPrinter (EXPLAIN)
      SeqScan → HashJoin → Filter → HashAggregate → Project → Distinct → Sort → Limit
      │  each operator: next() → Batch (columnar, pull model)
      ▼
   QueryResult   (schema + rows)
```

Full walkthrough for contributors: [docs/architecture.md](docs/architecture.md).

---

## Documentation

- **[docs/architecture.md](docs/architecture.md)** — the query lifecycle, every
  component, and the key design decisions.
- **[docs/sql-reference.md](docs/sql-reference.md)** — the exact SQL grammar and
  semantics LiteQuery supports.
- **[docs/c-api.md](docs/c-api.md)** — the C API reference, ownership rules, and
  threading model.

---

## Roadmap

Ordered roughly by impact:

1. **Typed columnar buffers** — replace `std::vector<Value>` columns with typed,
   page-based buffers + an Arrow-style validity bitmap; make operators run on
   typed arrays. This is the headline performance milestone.
2. **Unify the optimizer with execution** — carry real (cloned) expressions in
   the logical plan and drive the physical operators from the optimized plan so
   predicate pushdown / column pruning actually reach the executor.
3. **`HAVING`, correlated subqueries, `INSERT … SELECT`, set operations** at the
   execution layer (the planner already models `UNION`).
4. **Parquet & CSV ingestion** with zone-map predicate pushdown.
5. **Dictionary + RLE compression** per column.
6. **TPC-H benchmark harness** vs SQLite (the numbers that earn the columnar
   claim its keep).
7. **Python (pybind11 → PyArrow) and Rust (bindgen) bindings.**

---

## License

MIT — see [LICENSE](LICENSE).
