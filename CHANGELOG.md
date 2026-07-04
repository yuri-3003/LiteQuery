# Changelog

All notable changes to LiteQuery are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **`UPDATE` and `DELETE`**: `UPDATE t SET col = expr [, …] [WHERE p]` (SET
  expressions see the pre-update row values) and `DELETE FROM t [WHERE p]`; both
  report rows affected. New `UpdateStmt`/`DeleteStmt` AST nodes + parser.
- **SQL completeness batch**:
  - `HAVING` with aggregate expressions (`HAVING SUM(x) > 10`), including
    aggregates not present in the SELECT list.
  - `ORDER BY` bare aggregates (`ORDER BY SUM(x)` — no alias needed).
  - **Expressions over aggregates** in the SELECT list
    (`SELECT SUM(x)/COUNT(*)`, `MAX(v) - MIN(v)`).
  - `UNION` / `UNION ALL` execution via a new `Append` operator (`UNION`
    deduplicates; column-count mismatch is a clean error).
  - `INSERT … SELECT`.
  Implemented by rewriting aggregate-bearing expressions into references to
  aggregate output slots (each distinct aggregate computed once), with a real
  final projection for aggregate queries.

### Fixed
- **`UNION` was silently dropped**: `SELECT … UNION ALL SELECT …` previously
  returned only the left side's rows with no error. Both branches now execute.
- Aggregate queries now project exactly the SELECT list — previously the
  aggregate output (group keys + all aggregates, incl. HAVING-only ones) leaked
  into the result shape.

### Added (previous batch)
- **Persistence** — save/load a whole database (all tables + data) to a single
  file. New `Connection::saveDatabase`/`loadDatabase`, C API `lq_save`/`lq_load`,
  shell `.save`/`.open`, and `save`/`load` in the Python and Rust bindings. The
  on-disk format serializes the typed columns directly (data + validity bitmap),
  so all types and NULLs round-trip exactly; it is versioned and refuses to load
  a newer format. `storage/persistence.{h,cpp}`; 5 C++ tests + binding tests.
- **Rust bindings** (`bindings/rust/`): a safe crate over the C API. Its
  `build.rs` compiles the whole engine (via the `cc` crate) and links it in, so
  there is no separate library to install — `cargo build` is self-contained.
  `Connection::open()` with `query`/`execute`/`import_csv`; a `QueryResult` that
  iterates `Row` views (`get`/`get_str`/`get_i64`/`get_f64`), a `Value` enum,
  RAII cleanup, and `Result`-based errors. 7 integration tests + a doctest + a
  runnable `demo` example.

## [0.2.0] — 2026-07-03

Product milestone: LiteQuery goes from "a working engine" to "a tool people can
use" — an interactive shell, CSV ingestion, a typed columnar fast path that
beats SQLite on analytics, CI/packaging, and Python bindings.

### Added
- **Python bindings** (`bindings/python/`): pure-`ctypes` wrapper over the C
  API — no pybind11/Cython, works with any CPython. `litequery.connect()` →
  `Connection` with `query()` / `execute()` / `import_csv()`; a `Result` that
  iterates `Row`s (dict + positional access) with `.scalar()`, `.tuples()`,
  `.to_pandas()`. `pip install .` ships a **self-contained** shared library
  (statically-linked C++ runtime) so it loads regardless of how CPython was
  built. 9 unittest tests.
- **Shared library target** (`litequery_shared`, on by default): builds
  `liblitequery.{dll,so,dylib}` exposing the C API for FFI consumers.
- **`lq_import_csv`** added to the C API so C and Python callers can load CSVs.
- **LiteQuery-vs-SQLite benchmark** (`lq_vs_sqlite`, opt-in via
  `-DLITEQUERY_BUILD_SQLITE_BENCH=ON`): runs identical data and queries through
  both engines, reports both timings, and cross-checks that every result agrees.
  LiteQuery is 3–7× faster on the analytical aggregations; SQLite wins
  `COUNT(*)` (answered from metadata). SQLite 3.46.1 is vendored under
  `third_party/sqlite/` (public domain) and compiled only for this benchmark —
  it is not a dependency of the LiteQuery library.
- **Typed columnar storage**: columns now store contiguous typed buffers
  (`int64`/`double`/`string`) plus a 1-bit-per-row validity bitmap, instead of
  boxed `std::vector<Value>`. A `double` column is ~8 bytes/value + 1 bit versus
  ~40 bytes boxed. The `Table`/`Column` value API is unchanged.
- **Typed vectorized execution** for the common aggregate shape
  (`SELECT [key,] AGG(col)… FROM t [WHERE simple] [GROUP BY key]`): runs directly
  over the typed column arrays with no boxing or per-row variant dispatch, with
  a safe fallback to the general operator tree for anything that doesn't match.
  Measured 3–26× faster than the boxed path (see `bench/`).
- **Benchmark harness** (`lq_bench`): times full-scan / filtered / grouped
  aggregates over a generated table; `bench/README.md` documents the numbers.
- **CSV/TSV ingestion**: `Connection::importCsv()` and the shell's `.import FILE
  TABLE` load a delimited file into a new table, inferring column names (header)
  and types per column. Handles RFC-4180 quoting, `""` escapes, embedded
  newlines, CRLF, a leading UTF-8 BOM, and blank-cell → NULL.
- **`lq` interactive SQL shell**: a REPL over the engine with multi-line
  statements, `table`/`csv`/`json`/`list` output modes, and `.help` / `.tables`
  / `.schema` / `.import` / `.mode` / `.timing` / `.read` / `.quit`
  dot-commands. Runs files (`lq script.sql`), stdin pipes, and one-shot
  `-c "SQL"`.

### Fixed
- `ORDER BY` and `LIMIT` are now applied to aggregate (`GROUP BY`) results.
  Previously an aggregate query returned rows in hash order and ignored a
  trailing `ORDER BY`/`LIMIT`. Ordering by a group-key column or an aliased
  aggregate (`SUM(x) AS total … ORDER BY total`) now works.
- GitHub Actions CI: build + test on Linux (GCC/Clang), macOS, and Windows,
  plus an AddressSanitizer/UBSan job on Linux.
- CMake install/export: `find_package(LiteQuery)` provides the imported target
  `LiteQuery::litequery` for downstream projects.
- `CONTRIBUTING.md`, `CHANGELOG.md`, issue/PR templates.
- One-command build scripts (`build.sh`, `build.ps1`).
- Product roadmap (`docs/product-roadmap.md`).

## [0.1.0] — 2026-07-03

First working release: an embeddable columnar SQL query engine in C++17.

### Added
- **Front-end:** SQL lexer, recursive-descent + Pratt parser, `std::variant`
  AST with a visitor.
- **Planner & optimizer:** AST → logical relational-algebra plan; rule-based
  fixed-point optimizer (constant folding, predicate simplification, predicate
  pushdown, outer-join simplification, column pruning, project merge, limit
  pushdown); `EXPLAIN` via a plan printer.
- **Storage:** column-oriented in-memory `Table`/`Column`; thread-safe
  `Catalog`.
- **Execution:** scalar expression evaluator with full SQL three-valued NULL
  logic; vectorized pull-model operators (SeqScan, Filter, Project, Limit,
  Sort, Distinct, HashAggregate, HashJoin, Values).
- **API:** `lq::Connection` C++ pipeline and a pure-C public API
  (`lq_open`/`lq_query`/`lq_result_*`) with an exception-safe boundary.
- **SQL support:** `CREATE TABLE`, `DROP TABLE`, `INSERT … VALUES`; `SELECT`
  with projections/aliases, `WHERE`, `GROUP BY` with `COUNT`/`SUM`/`AVG`/`MIN`/
  `MAX` (and `COUNT(DISTINCT)`), `JOIN` (INNER/LEFT/RIGHT/FULL/CROSS with `ON`),
  `DISTINCT`, `ORDER BY` (including by dropped columns), `LIMIT`/`OFFSET`;
  arithmetic, comparisons, `AND`/`OR`/`NOT`, `||`, `LIKE`/`ILIKE`, `IN`,
  `BETWEEN`, `CASE`, `CAST`, and scalar functions (`COALESCE`, `ABS`, `LENGTH`,
  `UPPER`, `LOWER`, `ROUND`).
- **Build & tests:** CMake build producing a static library, a demo, and two
  test suites (26 C++ cases + a pure-C API test), all passing.
- **Docs:** README, architecture guide, SQL reference, C API reference.

### Known limitations
- In-memory only (no persistence yet).
- Values are boxed (`std::vector<Value>`) — the typed-buffer performance path is
  not yet implemented.
- `HAVING`, subqueries in `WHERE`/`FROM`, `INSERT … SELECT`, and set-operation
  execution are partial or unimplemented.
- No CSV/Parquet ingestion and no language bindings yet.

[Unreleased]: https://github.com/yuri-3003/LiteQuery/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/yuri-3003/LiteQuery/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/yuri-3003/LiteQuery/releases/tag/v0.1.0
