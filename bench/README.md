# LiteQuery benchmarks

`lq_bench` measures the analytical hot paths — full-scan aggregate, filtered
aggregate, and `GROUP BY` — over a randomly generated table held in memory.

```bash
cmake --build build --target lq_bench
./build/lq_bench            # 5,000,000 rows (default)
./build/lq_bench 20000000   # custom row count
```

It reports wall-clock time and throughput (rows/sec) for each query.

## What it measures

The table has three columns: `id BIGINT`, `cat INT` (10 distinct groups), and
`x DOUBLE`. The queries exercise the typed vectorized execution path:

| Query | Shape |
|---|---|
| `SELECT COUNT(*) FROM t` | full scan, count |
| `SELECT SUM(x) FROM t` | full scan, sum |
| `SELECT AVG(x) FROM t WHERE x > 500` | filtered aggregate |
| `SELECT cat, SUM(x) FROM t GROUP BY cat` | grouped aggregate |
| `SELECT cat, COUNT(*), SUM(x) FROM t WHERE x > 250 GROUP BY cat` | filtered group-by |

## Representative numbers

Measured at 5,000,000 rows, single thread, `-O2`, mingw-w64 GCC on a laptop.
Absolute numbers vary by machine; the **relative** jump from the boxed
(`std::vector<Value>`) execution to the typed columnar path is the point.

| Query | Boxed execution | Typed columnar | Speedup |
|---|---|---|---|
| `COUNT(*)` | ~12 M rows/s | ~306 M rows/s | ~26× |
| `SUM(x)` | ~14 M rows/s | ~241 M rows/s | ~17× |
| `AVG(x) WHERE x>500` | ~8 M rows/s | ~90 M rows/s | ~10× |
| `GROUP BY cat SUM(x)` | ~7 M rows/s | ~23 M rows/s | ~3× |
| filtered `GROUP BY` | ~5 M rows/s | ~27 M rows/s | ~5× |

`COUNT`/`SUM` become memory-bandwidth-bound. `GROUP BY` gains less because the
current group key is hashed as a string; an integer-keyed group path is the next
optimization.

## Head-to-head vs SQLite

`lq_vs_sqlite` runs the same data and the same queries through both LiteQuery and
a **vendored SQLite** (3.46.1, in-memory, loaded in one transaction), reports
both timings, and **verifies the two engines agree** on every result.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLITEQUERY_BUILD_SQLITE_BENCH=ON
cmake --build build --target lq_vs_sqlite
./build/lq_vs_sqlite            # 3,000,000 rows (default)
```

Representative run at 5,000,000 rows (single thread, `-O2`, mingw-w64 GCC):

| Query | LiteQuery | SQLite | Speedup | Result |
|---|---|---|---|---|
| load | 0.28 s | 0.92 s | 3.2× | — |
| `COUNT(*)` | 0.017 s | 0.001 s | **0.1×** | ✓ match |
| `SUM(x)` | 0.021 s | 0.118 s | **5.6×** | ✓ match |
| `AVG(x) WHERE x>500` | 0.054 s | 0.181 s | **3.3×** | ✓ match |
| `GROUP BY cat SUM(x)` | 0.220 s | 1.395 s | **6.3×** | ✓ match |
| filtered `GROUP BY` | 0.200 s | 1.293 s | **6.5×** | ✓ match |

LiteQuery is **3–7× faster on the analytical aggregations** it's built for, on a
columnar scan of the whole table. **SQLite wins `COUNT(*)`** decisively — it
answers from metadata rather than scanning, which LiteQuery doesn't special-case
yet. Reporting that honestly is the point: this is a real comparison, not a
cherry-pick, and every LiteQuery result matches SQLite to within floating-point
tolerance.

> SQLite is compiled only when `-DLITEQUERY_BUILD_SQLITE_BENCH=ON`; it is not a
> dependency of the LiteQuery library. See `third_party/sqlite/README.md`.
