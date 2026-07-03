// ============================================================================
// LiteQuery — bench/bench.cpp
// Micro-benchmarks for the analytical hot paths: full-scan aggregate, filtered
// aggregate, and GROUP BY. Generates a table in memory, then times queries.
//
//   lq_bench [rows]        default 5,000,000 rows
//
// Reports wall-clock time and throughput (rows/sec) per query.
// ============================================================================

#include "connection.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace lq;
using Clock = std::chrono::steady_clock;

static double seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

int main(int argc, char** argv) {
    size_t rows = 5'000'000;
    if (argc > 1) rows = std::strtoull(argv[1], nullptr, 10);

    std::printf("LiteQuery benchmark — %zu rows\n", rows);
    std::printf("============================================\n");

    Connection db;
    db.query("CREATE TABLE t (id BIGINT, cat INT, x DOUBLE)");

    // ---- Build the data directly in the table (bypass SQL INSERT parsing) ----
    auto t0 = Clock::now();
    {
        std::mt19937_64 rng(42);
        std::uniform_int_distribution<int> catDist(0, 9);      // 10 groups
        std::uniform_real_distribution<double> xDist(0.0, 1000.0);

        TablePtr tbl = db.catalog().getTable("t");
        std::vector<std::vector<Value>> cols(3);
        for (auto& c : cols) c.reserve(rows);
        for (size_t i = 0; i < rows; ++i) {
            cols[0].push_back(Value(static_cast<int64_t>(i)));
            cols[1].push_back(Value(static_cast<int32_t>(catDist(rng))));
            cols[2].push_back(Value(xDist(rng)));
        }
        tbl->bulkInsertColumns(std::move(cols));
    }
    auto t1 = Clock::now();
    std::printf("load:              %8.3f s  (%6.1f M rows/s)\n",
                seconds(t0, t1), rows / seconds(t0, t1) / 1e6);

    struct Q { const char* name; const char* sql; };
    const std::vector<Q> queries = {
        {"count(*)",            "SELECT COUNT(*) FROM t"},
        {"sum(x)",              "SELECT SUM(x) FROM t"},
        {"avg(x) where x>500",  "SELECT AVG(x) FROM t WHERE x > 500"},
        {"group by cat sum(x)", "SELECT cat, SUM(x) FROM t GROUP BY cat"},
        {"filtered group by",   "SELECT cat, COUNT(*), SUM(x) FROM t WHERE x > 250 GROUP BY cat"},
    };

    for (const auto& q : queries) {
        // Warm + timed run (single run; the dataset is large enough to be stable).
        auto a = Clock::now();
        QueryResult r = db.query(q.sql);
        auto b = Clock::now();
        double s = seconds(a, b);
        if (!r.ok()) {
            std::printf("%-22s ERROR: %s\n", q.name, r.errorMessage.c_str());
            continue;
        }
        std::printf("%-22s %8.3f s  (%6.1f M rows/s)  -> %zu result row(s)\n",
                    q.name, s, rows / s / 1e6, r.rows.size());
    }

    return 0;
}
