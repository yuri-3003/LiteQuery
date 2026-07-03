// ============================================================================
// LiteQuery — bench/bench_sqlite.cpp
// Head-to-head: LiteQuery vs SQLite on identical data and queries.
//
//   lq_vs_sqlite [rows]      default 3,000,000 rows
//
// Both engines get the same randomly generated table (same seed) loaded via
// their bulk paths, then run the same analytical queries. For each query we
// report both timings, the speedup, and whether the two engines agree on the
// result (a correctness cross-check, not just a race).
//
// SQLite is an in-memory database (":memory:") loaded inside one transaction,
// which is the fastest supported insert path — a fair comparison target.
// ============================================================================

#include "connection.h"
#include "sqlite3.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace lq;
using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// Generated row.
struct RowData { int64_t id; int cat; double x; };

static std::vector<RowData> generate(size_t n) {
    std::vector<RowData> v;
    v.reserve(n);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> catDist(0, 9);
    std::uniform_real_distribution<double> xDist(0.0, 1000.0);
    for (size_t i = 0; i < n; ++i)
        v.push_back({static_cast<int64_t>(i), catDist(rng), xDist(rng)});
    return v;
}

// A single scalar (double) result, or NaN.
static double lqScalar(Connection& db, const char* sql, size_t& rowsOut) {
    QueryResult r = db.query(sql);
    rowsOut = r.rows.size();
    if (!r.ok() || r.rows.empty()) return std::nan("");
    return r.rows[0][0].isNull() ? std::nan("") : r.rows[0][0].toDouble();
}

static double sqliteScalar(sqlite3* db, const char* sql, size_t& rowsOut) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return std::nan("");
    double first = std::nan("");
    rowsOut = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (rowsOut == 0) first = sqlite3_column_double(st, 0);
        ++rowsOut;
    }
    sqlite3_finalize(st);
    return first;
}

// Sum column 1 over all result rows (used to compare GROUP BY results without
// depending on row order).
static double lqSumCol1(Connection& db, const char* sql, size_t& rowsOut) {
    QueryResult r = db.query(sql);
    rowsOut = r.rows.size();
    double s = 0;
    for (auto& row : r.rows) if (!row[1].isNull()) s += row[1].toDouble();
    return s;
}
static double sqliteSumCol1(sqlite3* db, const char* sql, size_t& rowsOut) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return std::nan("");
    double s = 0; rowsOut = 0;
    while (sqlite3_step(st) == SQLITE_ROW) { s += sqlite3_column_double(st, 1); ++rowsOut; }
    sqlite3_finalize(st);
    return s;
}

static bool approxEq(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    double diff = std::fabs(a - b);
    double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    return diff <= 1e-6 * scale;
}

int main(int argc, char** argv) {
    size_t rows = 3'000'000;
    if (argc > 1) rows = std::strtoull(argv[1], nullptr, 10);

    std::printf("LiteQuery vs SQLite %s  —  %zu rows\n", sqlite3_libversion(), rows);
    std::printf("============================================================\n");

    auto data = generate(rows);

    // ---- Load LiteQuery ----------------------------------------------------
    Connection lq;
    lq.query("CREATE TABLE t (id BIGINT, cat INT, x DOUBLE)");
    auto l0 = Clock::now();
    {
        TablePtr tbl = lq.catalog().getTable("t");
        std::vector<std::vector<Value>> cols(3);
        for (auto& c : cols) c.reserve(rows);
        for (const auto& d : data) {
            cols[0].push_back(Value(d.id));
            cols[1].push_back(Value(static_cast<int32_t>(d.cat)));
            cols[2].push_back(Value(d.x));
        }
        tbl->bulkInsertColumns(std::move(cols));
    }
    auto l1 = Clock::now();

    // ---- Load SQLite (in-memory, single transaction) -----------------------
    sqlite3* sq = nullptr;
    sqlite3_open(":memory:", &sq);
    char* err = nullptr;
    sqlite3_exec(sq, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;", nullptr, nullptr, &err);
    sqlite3_exec(sq, "CREATE TABLE t (id INTEGER, cat INTEGER, x REAL)", nullptr, nullptr, &err);
    auto s0 = Clock::now();
    {
        sqlite3_exec(sq, "BEGIN", nullptr, nullptr, &err);
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(sq, "INSERT INTO t VALUES (?,?,?)", -1, &ins, nullptr);
        for (const auto& d : data) {
            sqlite3_bind_int64(ins, 1, d.id);
            sqlite3_bind_int(ins, 2, d.cat);
            sqlite3_bind_double(ins, 3, d.x);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        sqlite3_finalize(ins);
        sqlite3_exec(sq, "COMMIT", nullptr, nullptr, &err);
    }
    auto s1 = Clock::now();

    std::printf("%-26s  LiteQuery      SQLite       speedup\n", "");
    std::printf("%-26s  %8.3f s     %8.3f s     %5.1fx\n", "load",
                secs(l0, l1), secs(s0, s1), secs(s0, s1) / secs(l0, l1));
    std::printf("------------------------------------------------------------\n");

    struct Q { const char* name; const char* sql; bool groupBy; };
    const std::vector<Q> queries = {
        {"COUNT(*)",              "SELECT COUNT(*) FROM t", false},
        {"SUM(x)",                "SELECT SUM(x) FROM t", false},
        {"AVG(x) WHERE x>500",    "SELECT AVG(x) FROM t WHERE x > 500", false},
        {"GROUP BY cat SUM(x)",   "SELECT cat, SUM(x) FROM t GROUP BY cat", true},
        {"filtered GROUP BY",     "SELECT cat, SUM(x) FROM t WHERE x > 250 GROUP BY cat", true},
    };

    int mismatches = 0;
    for (const auto& q : queries) {
        size_t lqRows = 0, sqRows = 0;
        double lqVal, sqVal;

        auto a = Clock::now();
        if (q.groupBy) lqVal = lqSumCol1(lq, q.sql, lqRows);
        else           lqVal = lqScalar(lq, q.sql, lqRows);
        auto b = Clock::now();

        auto c = Clock::now();
        if (q.groupBy) sqVal = sqliteSumCol1(sq, q.sql, sqRows);
        else           sqVal = sqliteScalar(sq, q.sql, sqRows);
        auto d = Clock::now();

        double lqS = secs(a, b), sqS = secs(c, d);
        bool agree = approxEq(lqVal, sqVal) && lqRows == sqRows;
        if (!agree) ++mismatches;

        std::printf("%-26s  %8.3f s     %8.3f s     %5.1fx  %s\n",
                    q.name, lqS, sqS, sqS / lqS, agree ? "= match" : "! MISMATCH");
    }

    sqlite3_close(sq);
    std::printf("------------------------------------------------------------\n");
    if (mismatches == 0)
        std::printf("All results match SQLite.\n");
    else
        std::printf("%d result mismatch(es) vs SQLite!\n", mismatches);
    return mismatches == 0 ? 0 : 1;
}
