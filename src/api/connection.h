#pragma once

// ============================================================================
// LiteQuery — connection.h
// The C++ entry point for executing SQL against an in-memory database.
//
// A Connection owns a Catalog and runs each query through this pipeline:
//
//   SQL text
//     -> Lexer::tokenize()      -> tokens
//     -> Parser::parseStatement -> ast::StmtNode
//     -> dispatch by statement kind:
//          SELECT           build an operator tree (SeqScan / Join / Filter /
//                           HashAggregate / Project / Distinct / Sort / Limit)
//                           and drain it into a QueryResult
//          INSERT/UPDATE/DELETE   mutate the target table
//          CREATE/DROP            update the catalog
//
// query() never throws: lex/parse/eval exceptions are caught and returned in
// QueryResult::error. EXPLAIN prints the same operator tree the executor runs.
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

    // Load a CSV/TSV file into a new table `tableName`, inferring column names
    // (from the header) and types. Returns a result whose rowsAffected is the
    // number of rows loaded; on failure returns an error result (never throws).
    // `delimiter` is ',' for CSV or '\t' for TSV; `hasHeader` names the columns
    // from the first row when true.
    QueryResult importCsv(const std::string& path,
                          const std::string& tableName,
                          char delimiter = ',',
                          bool hasHeader = true);

    // Save the whole database (all tables + data) to a single file, and load it
    // back. Snapshot semantics: save() writes the current catalog; load() adds
    // the file's tables (replacing same-named ones). Never throw — errors come
    // back in the result's error/errorMessage.
    QueryResult saveDatabase(const std::string& path);
    QueryResult loadDatabase(const std::string& path);

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
