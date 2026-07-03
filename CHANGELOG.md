# Changelog

All notable changes to LiteQuery are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
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

[Unreleased]: https://github.com/yuri-3003/LiteQuery/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/yuri-3003/LiteQuery/releases/tag/v0.1.0
