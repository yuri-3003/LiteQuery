# Vendored SQLite (for benchmarks only)

This directory contains the [SQLite](https://sqlite.org) amalgamation
(`sqlite3.c` + `sqlite3.h`), version **3.46.1**, used **only** by the
`lq_vs_sqlite` benchmark to compare LiteQuery against SQLite on identical data
and queries.

- It is **not** part of the LiteQuery library — the engine and its public C API
  remain zero-dependency. SQLite is compiled only when
  `-DLITEQUERY_BUILD_SQLITE_BENCH=ON` (off by default).
- **License:** SQLite is in the **public domain**
  (<https://sqlite.org/copyright.html>). No attribution is required; this note
  is courtesy.

To update: download a newer amalgamation ZIP from
<https://sqlite.org/download.html>, extract `sqlite3.c` and `sqlite3.h`, and
replace the files here.
