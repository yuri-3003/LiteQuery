#pragma once

// ============================================================================
// LiteQuery — connection.h
// The C++ entry point for executing SQL against an in-memory database.
//
// A Connection owns a Catalog and runs the full pipeline for each query:
//
//   SQL text
//     │  Lexer::tokenize()
//     ▼  tokens
//     │  Parser::parseStatement()
//     ▼  ast::StmtNode
//     │  dispatch: CREATE / DROP / INSERT / SELECT
//     ▼
//   SELECT →  build operator tree (SeqScan → Join → Filter → Aggregate →
//             Project → Distinct → Sort → Limit)  →  drain to QueryResult
//   CREATE →  register table in catalog
//   INSERT →  evaluate rows, append to table
//
// Errors are never thrown across query(): the pipeline's exceptions (lex/parse/
// plan/eval) are caught and returned in QueryResult::error. The optimizer and
// PlanPrinter are available for EXPLAIN, but result correctness flows through
// the operator tree built directly from the resolved AST.
//
// Thread-safety: a Connection is not thread-safe; use one per thread. Multiple
// Connections may share a Catalog (its shared_mutex guards concurrent access).
// ============================================================================

#include "catalog.h"
#include "types.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lq {

// ============================================================================
// QueryResult — the materialized outcome of a query
// ============================================================================

struct QueryResult {
    bool                error = false;
    std::string         errorMessage;

    Schema              schema;              // Column names/types of the result
    std::vector<Row>    rows;                // Result rows (empty for DDL/DML)
    int64_t             rowsAffected = 0;    // For INSERT/CREATE/DROP

    // Timing (microseconds), filled best-effort.
    int64_t             elapsedMicros = 0;

    bool ok() const noexcept { return !error; }
    size_t rowCount() const noexcept { return rows.size(); }
    size_t columnCount() const noexcept { return schema.size(); }

    static QueryResult makeError(std::string msg) {
        QueryResult r;
        r.error = true;
        r.errorMessage = std::move(msg);
        return r;
    }
};

// ============================================================================
// Connection
// ============================================================================

class Connection {
public:
    Connection();
    ~Connection();

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    // Movable so a Connection can be returned from a factory function.
    Connection(Connection&&) noexcept            = default;
    Connection& operator=(Connection&&) noexcept = default;

    // Execute one SQL statement and return the result. Never throws.
    QueryResult query(const std::string& sql);

    // Produce an EXPLAIN-style textual plan for a SELECT. Never throws; returns
    // the error text on failure.
    std::string explain(const std::string& sql);

    // Direct catalog access (for programmatic table registration / bulk load).
    Catalog&       catalog()       noexcept { return *catalog_; }
    const Catalog& catalog() const noexcept { return *catalog_; }

private:
    std::unique_ptr<Catalog> catalog_;
};

}  // namespace lq
