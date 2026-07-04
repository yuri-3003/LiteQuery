# LiteQuery — Product Roadmap

**From working v0.1 engine → shippable open-source developer product.**

This is the build plan. Phases are ordered by dependency and impact. We build
one phase at a time; each phase ends with something demonstrable and committed.

> Legend: ✅ done · 🔨 in progress · ⬜ planned

---

## Where we are (v0.1 — shipped)

The engine works end-to-end and is committed/pushed:

- Lexer, parser, logical planner, rule-based optimizer
- Columnar storage, expression evaluator, pull-model operators
- Connection pipeline + public C API
- CMake build, 26 C++ tests + pure-C test (all green), demo, docs

**What blocks it from being a *product* people adopt:** no persistence, no data
ingestion, not actually fast yet, in-memory only, no packaging/CI, no bindings.

---

## Phase 0 — Product foundation & credibility  ✅
*Goal: make the repo look and behave like a real product so early visitors trust it.*
*Effort: S (1 session). Depends on: nothing.*

- [ ] GitHub repo polish: description, topics, `master`→`main` (optional), badges
- [ ] CI: GitHub Actions building + running CTest on Linux/macOS/Windows
- [ ] `CONTRIBUTING.md`, `CHANGELOG.md`, issue/PR templates
- [ ] Version tag `v0.1.0` + a GitHub Release with notes
- [ ] `CMake install` + a `litequeryConfig.cmake` so downstream `find_package` works
- [ ] A one-command build script and a "Try it in 60 seconds" README section

**Unlocks:** every later phase can rely on green CI; the project reads as maintained.

---

## Phase 1 — Interactive SQL shell (the "wow" in 30 seconds)  ✅
*Goal: a REPL binary so anyone can type SQL and see results without writing code.*
*Effort: S–M. Depends on: Phase 0 (nice-to-have).*

- [ ] `lq` CLI: read-eval-print loop over `Connection`
- [ ] Pretty table output, timing, `.tables` / `.schema` / `.help` meta-commands
- [ ] Run a `.sql` file; pipe SQL from stdin
- [ ] `.read` / `.mode` (table | csv | json) output formats
- [ ] Ship it in the README as the headline demo (asciinema/GIF)

**Unlocks:** a shareable artifact; the fastest path to a GitHub star.

---

## Phase 2 — Data ingestion: CSV  ✅
*Goal: point LiteQuery at real datasets instead of hand-written INSERTs.*
*Effort: M. Depends on: nothing (storage API is ready).*

- [ ] `csv_reader`: type inference, quoted fields, CRLF, BOM, headers, TSV
- [ ] SQL: `COPY t FROM 'file.csv'` and/or `CREATE TABLE t AS SELECT … FROM read_csv('…')`
- [ ] CLI: `.import file.csv table`
- [ ] Bulk columnar load path (append whole columns, not row-by-row)
- [ ] Tests against a few real-world CSVs

**Unlocks:** demos on actual data (NYC taxi, etc.); makes benchmarks possible.

---

## Phase 3 — Performance: typed columnar execution  ✅ (core)
*Goal: deliver the columnar speed the pitch promises. The headline milestone.*
*Effort: L. Depends on: Phase 2 (need real data to measure).*

- [x] Replace `vector<Value>` columns with typed buffers + validity bitmap
- [x] Typed aggregate fast path (scan/filter/group over `int64*`/`double*`)
- [x] Micro-benchmarks proving the speedup (3–26× vs the boxed path)
- [ ] Integer-keyed GROUP BY (skip string hashing) — next perf win
- [ ] Vectorized predicate eval + zone maps / min-max scan skipping

**Delivered:** the common aggregate shape now runs typed & vectorized, 3–26×
faster than the boxed path. Remaining items push GROUP BY and scans further; the
SQLite comparison lands in Phase 4.

---

## Phase 4 — Benchmarks vs SQLite (proof)  ✅
*Goal: publish numbers that back the columnar claim.*
*Effort: M. Depends on: Phase 3.*

- [x] LiteQuery vs SQLite on identical data + aggregation/group-by queries
- [x] Result cross-check (both engines must agree) built into the harness
- [x] Results table committed to the README + `bench/README.md`
- [x] Reproducible with a single CMake option + command
- [ ] TPC-H schema + join queries at SF 0.1 / SF 1 (broader coverage, later)

**Delivered:** LiteQuery is 3–7× faster than SQLite on the analytical
aggregations, with every result verified equal; SQLite wins `COUNT(*)` (metadata
answer), reported honestly. Vendored SQLite 3.46.1, opt-in build.

---

## Phase 5 — Persistence  ✅ (core)
*Goal: databases survive process exit — usable for real, not just in-memory.*
*Effort: L. Depends on: Phase 3 (persist the typed format).*

- [x] Save/load a database to a single file (columnar on-disk format)
- [x] Format versioning (refuses to load a newer format)
- [x] Exposed everywhere: C++/C API, shell `.save`/`.open`, Python, Rust
- [ ] File-backed `open("mydb.lq")` with incremental/append-safe writes (future)

**Delivered:** the whole catalog round-trips to one file with all types and NULLs
preserved (typed columns serialized directly). Snapshot semantics for now;
incremental/append-safe writes are a later refinement.

---

## Phase 6 — SQL completeness  ⬜
*Goal: close the gaps that make users hit "not supported" walls.*
*Effort: M–L. Depends on: nothing hard, but best after the executor is unified.*

- [ ] `HAVING` fully wired over aggregate output
- [ ] Subqueries in `WHERE` / `FROM`; scalar subqueries; `IN (subquery)`
- [ ] `INSERT … SELECT`; `UPDATE`; `DELETE`
- [ ] Set operations execution: `UNION`/`UNION ALL`/`INTERSECT`/`EXCEPT`
- [ ] More scalar/date functions; `CAST` `::` shorthand
- [ ] Unify optimizer's logical plan with the execution path

**Unlocks:** "it just works" for the queries developers actually write.

---

## Phase 7 — Language bindings  🔨 (Python + Rust done)
*Goal: make LiteQuery usable from the languages data people live in.*
*Effort: M each. Depends on: stable C API (already have it).*

- [x] Python: `ctypes` wrapper over the C API — `Connection.query()` →
      rows/`.to_pandas()`; `import_csv`; self-contained shared library so no
      pybind11/Cython and any CPython works; 9 tests
- [x] Rust: safe crate over the C API; `build.rs` compiles the engine in (no
      separate lib to install); `Connection`/`QueryResult`/`Row`/`Value`, RAII,
      `Result` errors; 7 tests + doctest + demo
- [ ] Publish to PyPI (per-platform wheels in CI: `cibuildwheel`) + crates.io
- [ ] Node/WASM (stretch): compile to WASM, run in the browser

**Delivered:** working, dependency-free Python and Rust bindings from a source
checkout. Publishing to the package registries remains (a CI/release task).

---

## Phase 8 — Launch & distribution  ⬜
*Goal: put it in front of people and make it trivial to install.*
*Effort: M. Depends on: Phases 1–4 minimum for a credible launch.*

- [ ] Prebuilt binaries per-platform on each release (CI artifacts)
- [ ] Package managers: Homebrew tap, vcpkg/Conan port
- [ ] A small docs site (GitHub Pages) from the `docs/` markdown
- [ ] Launch write-up (blog/HN/Reddit) leading with the benchmark
- [ ] `litequery.dev` landing page (optional)

**Unlocks:** discovery, installs, and the feedback loop that grows a project.

---

## Suggested build order

The fastest path to "a product people try and share":

**0 → 1 → 2 → 3 → 4**, then **5 / 6 / 7 / 8** as adoption dictates.

Rationale: Phase 0 makes it credible, Phase 1 makes it *demoable*, Phase 2 lets
it touch real data, Phase 3 delivers the actual speed, and Phase 4 proves it.
Those five turn "a cool repo" into "a tool with a reason to exist." Persistence,
completeness, bindings, and launch follow once there's something worth adopting.
