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

> A LiteQuery-vs-SQLite comparison on TPC-H queries is Phase 4 of the
> [product roadmap](../docs/product-roadmap.md) — this harness is the in-tree
> baseline that makes that comparison meaningful.
