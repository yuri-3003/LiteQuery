#pragma once

// ============================================================================
// LiteQuery — fast_aggregate.h
// A typed, vectorized fast path for the most common analytical query shape:
//
//   SELECT [key,] AGG(col) [, AGG(col) ...]
//   FROM <single base table>
//   [WHERE <conjunction of  col <op> constant>]
//   [GROUP BY key]
//
// When a SELECT matches this shape, execution runs directly over the table's
// typed column buffers (int64/double arrays + validity bitmap) with no boxing,
// no per-row std::variant dispatch, and no intermediate Batch — the core of the
// columnar speedup. Anything that doesn't match falls back to the general
// operator tree, so correctness is never at risk.
// ============================================================================

#include "ast.h"
#include "connection.h"   // QueryResult
#include "table.h"
#include "types.h"

#include <optional>

namespace lq {

class Catalog;

// If `stmt` matches the fast-path shape and every referenced column exists,
// execute it over typed columns and return the result. Otherwise return
// std::nullopt so the caller uses the general executor.
std::optional<QueryResult> tryFastAggregate(const ast::SelectStmt& stmt,
                                             Catalog& catalog);

}  // namespace lq
