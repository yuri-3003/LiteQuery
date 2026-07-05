# Changelog

Notable changes to LiteQuery. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `UPDATE t SET col = expr [, ...] [WHERE p]` (SET expressions see the
  pre-update row values) and `DELETE FROM t [WHERE p]`; both report rows
  affected.
- `HAVING` with aggregate expressions, including aggregates not in the SELECT
  list.
- `ORDER BY` bare aggregates (`ORDER BY SUM(x)`) and expressions over aggregates
  in the SELECT list (`SELECT SUM(x)/COUNT(*)`, `MAX(v) - MIN(v)`).
- `UNION` / `UNION ALL` execution (`UNION` deduplicates; a column-count mismatch
  is an error).
- `INSERT ... SELECT`.
- Persistence: save/load a whole database to a single file. `Connection::
  saveDatabase`/`loadDatabase`, C API `lq_save`/`lq_load`, shell `.save`/`.open`,
  and `save`/`load` in the Python and Rust bindings. The format serializes the
  typed columns directly, so all types and NULLs round-trip; it is versioned and
  refuses to load a newer format.
- Rust bindings (`bindings/rust/`): a safe crate whose `build.rs` compiles the
  engine in, so there is nothing to install separately.

### Changed
- `EXPLAIN` now prints the actual operator tree the executor runs.

### Fixed
- `UNION`/`UNION ALL` previously returned only the left query's rows with no
  error; both branches now execute.
- Aggregate queries now project exactly the SELECT list rather than leaking
  group-key and internal aggregate columns into the result.

### Removed
- The unused logical-plan / rule-based-optimizer layer. Query execution builds
  the operator tree directly from the AST, and `EXPLAIN` prints that tree.

## [0.2.0] - 2026-07-03

### Added
- Typed columnar storage: columns are contiguous typed buffers
  (`int64`/`double`/`string`) plus a 1-bit-per-row validity bitmap.
- A typed vectorized path for the common aggregate shape (`SELECT [key,]
  AGG(col)... FROM t [WHERE simple] [GROUP BY key]`), with a fallback to the
  general operator tree for anything else. Several times faster than the general
  path (see `bench/`).
- `lq` interactive shell: multi-line statements, `table`/`csv`/`json`/`list`
  output modes, and `.help` / `.tables` / `.schema` / `.import` / `.mode` /
  `.timing` / `.read` / `.quit` dot-commands. Runs files, stdin pipes, and
  one-shot `-c "SQL"`.
- CSV/TSV ingestion with per-column type inference: `Connection::importCsv()`,
  the shell's `.import`, and the C API `lq_import_csv`. Handles RFC-4180
  quoting, `""` escapes, embedded newlines, CRLF, a UTF-8 BOM, and blank cells
  as NULL.
- Python bindings (`bindings/python/`): a pure-`ctypes` wrapper (no pybind11 or
  Cython) that works with any CPython. `pip install .` ships a self-contained
  shared library.
- Shared library target (`litequery_shared`) exposing the C API for FFI.
- Benchmarks: `lq_bench` (in-tree) and `lq_vs_sqlite` (opt-in via
  `-DLITEQUERY_BUILD_SQLITE_BENCH=ON`), which run identical data and queries
  through LiteQuery and a vendored SQLite and check that results agree.
- CI on Linux (GCC/Clang), macOS, and Windows, plus an ASan/UBSan job.
- CMake install/export: `find_package(LiteQuery)` provides
  `LiteQuery::litequery`. `CONTRIBUTING.md`, issue/PR templates, and
  one-command build scripts.

### Fixed
- `ORDER BY` and `LIMIT` are applied to aggregate (`GROUP BY`) results.

## [0.1.0] - 2026-07-03

First release: an embeddable columnar SQL query engine in C++17.

### Added
- SQL lexer, recursive-descent + Pratt parser, and a `std::variant` AST.
- Column-oriented in-memory storage and a thread-safe catalog.
- A scalar expression evaluator with SQL three-valued NULL logic, and pull-model
  operators (SeqScan, Filter, Project, Limit, Sort, Distinct, HashAggregate,
  HashJoin, Values).
- A `lq::Connection` C++ API and a pure-C public API
  (`lq_open`/`lq_query`/`lq_result_*`) with an exception-safe boundary.
- SQL: `CREATE TABLE`, `DROP TABLE`, `INSERT ... VALUES`; `SELECT` with
  projections/aliases, `WHERE`, `GROUP BY` with `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`,
  `JOIN` (INNER/LEFT/RIGHT/FULL/CROSS), `DISTINCT`, `ORDER BY`, `LIMIT`/`OFFSET`;
  arithmetic, comparisons, boolean logic, `LIKE`/`ILIKE`, `IN`, `BETWEEN`,
  `CASE`, `CAST`, and a few scalar functions.
- A CMake build with a test suite (C++ and pure-C).

[Unreleased]: https://github.com/yuri-3003/LiteQuery/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/yuri-3003/LiteQuery/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/yuri-3003/LiteQuery/releases/tag/v0.1.0
