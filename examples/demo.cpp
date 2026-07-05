// ============================================================================
// LiteQuery — examples/demo.cpp
// A small tour of the C++ API: create tables, load data, run analytical
// queries, and print results as a simple table. Also demonstrates EXPLAIN.
//
//   ./lq_demo
// ============================================================================

#include "connection.h"

#include <iomanip>
#include <iostream>
#include <string>

using namespace lq;

static void printResult(const QueryResult& r) {
    if (!r.ok()) {
        std::cout << "  ERROR: " << r.errorMessage << "\n";
        return;
    }
    if (r.schema.size() == 0) {
        std::cout << "  OK";
        if (r.rowsAffected) std::cout << " (" << r.rowsAffected << " rows affected)";
        std::cout << "\n";
        return;
    }
    // Header
    std::cout << "  ";
    for (size_t i = 0; i < r.schema.size(); ++i)
        std::cout << (i ? " | " : "") << r.schema[i].name;
    std::cout << "\n  " << std::string(40, '-') << "\n";
    // Rows
    for (const auto& row : r.rows) {
        std::cout << "  ";
        for (size_t i = 0; i < row.size(); ++i)
            std::cout << (i ? " | " : "") << row[i].toString();
        std::cout << "\n";
    }
    std::cout << "  (" << r.rows.size() << " rows, "
              << r.elapsedMicros << " us)\n";
}

static void run(Connection& c, const std::string& sql) {
    std::cout << "\nsql> " << sql << "\n";
    printResult(c.query(sql));
}

int main() {
    Connection db;

    std::cout << "=== LiteQuery demo ===\n";

    run(db, "CREATE TABLE sales (id INT, region VARCHAR, product VARCHAR, amount DOUBLE)");
    run(db, "INSERT INTO sales VALUES "
            "(1,'West','Widget',100.0),(2,'West','Gadget',250.0),"
            "(3,'East','Widget',120.0),(4,'East','Gadget',80.0),"
            "(5,'West','Widget',60.0),(6,'East','Widget',200.0)");

    run(db, "SELECT * FROM sales");

    std::cout << "\n--- Analytical queries ---";
    run(db, "SELECT region, COUNT(*), SUM(amount), AVG(amount) "
            "FROM sales GROUP BY region");

    run(db, "SELECT product, SUM(amount) AS total "
            "FROM sales GROUP BY product ORDER BY total DESC");

    run(db, "SELECT region, product, amount FROM sales "
            "WHERE amount > 100 ORDER BY amount DESC LIMIT 3");

    run(db, "SELECT DISTINCT region FROM sales ORDER BY region");

    std::cout << "\n--- EXPLAIN (operator tree) ---\n";
    std::cout << db.explain(
        "SELECT region, SUM(amount) FROM sales WHERE amount > 50 GROUP BY region") << "\n";

    return 0;
}
