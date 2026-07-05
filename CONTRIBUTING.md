# Contributing to LiteQuery

Thanks for your interest! LiteQuery is an embeddable columnar SQL engine in
C++17 with zero external dependencies. This guide gets you productive fast.

## Getting set up

You need **CMake ≥ 3.16** and a **C++17** compiler (GCC, Clang, or MSVC).

```bash
git clone https://github.com/yuri-3003/LiteQuery.git
cd LiteQuery
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

All tests should pass before you start. If they don't on a clean checkout,
that's a bug — please open an issue.

## Project layout

See [docs/architecture.md](docs/architecture.md) for the full tour. In short:

```
include/litequery/   Public C API (litequery.h)
src/parser/          Lexer, AST, parser
src/catalog/         Table registry
src/storage/         Columns, tables, CSV, persistence
src/execution/       Expression evaluator, physical operators, fast path
src/api/             Connection pipeline + C API wrapper
tests/               Test framework + suites
```

**Where to make a change:**
- New SQL syntax: `src/parser/parser.cpp` (plus an AST node if needed).
- New expression or scalar function: `src/execution/eval.cpp`.
- New clause or operator: a new operator in `src/execution/physical_plan.*` and
  its wiring in `src/api/connection.cpp` (`buildSelect`).
- Faster query shape: extend `src/execution/fast_aggregate.cpp`.

## Coding standards

- C++17, no external dependencies. The standard library is fine; third-party
  libraries are not.
- Match the surrounding style: 4-space indent, `lowerCamelCase` for functions
  and variables, `PascalCase` for types, `snake_case` only in the C API.
- Prefer RAII and `unique_ptr`/`shared_ptr` over manual `new`/`delete`. The only
  place raw `new`/`delete` is acceptable is the C API handle wrappers.
- Keep the public C header (`litequery.h`) pure C, with no C++ leakage.
- Comment the why, not the what.

## Tests are required

Every behavioral change needs a test. The harness is `tests/test_framework.h`
(zero-dependency: `TEST`, `CHECK`, `CHECK_EQ`). Add cases to
`tests/test_litequery.cpp`; add C-boundary cases to `tests/test_capi.c`.

Run the suite with sanitizers when touching memory-sensitive code (Linux/macOS):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLITEQUERY_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## Pull requests

1. Fork and branch from `master` (`feature/...` or `fix/...`).
2. Keep PRs focused: one logical change per PR.
3. Make sure `ctest` passes locally; CI must be green (Linux/macOS/Windows).
4. Describe what and why in the PR body. Link any related issue.
5. Update docs (`docs/sql-reference.md`, etc.) if you change behavior.

## Reporting bugs

Open an issue with the SQL that misbehaves, what you expected, what you got, and
your platform and compiler. A minimal reproducer helps a lot.

By contributing, you agree your contributions are licensed under the project's
[MIT License](LICENSE).
