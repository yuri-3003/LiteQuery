#pragma once

// ============================================================================
// LiteQuery — eval.h
// Scalar expression evaluator: (ast::ExprNode, Schema, Row) → Value.
//
// The evaluator implements SQL semantics for scalar expressions:
//   - arithmetic with implicit numeric promotion (int → double),
//   - three-valued boolean logic (NULL propagation; NULL AND false = false),
//   - comparisons across compatible numeric types,
//   - LIKE / ILIKE with % and _ wildcards,
//   - CASE (simple & searched), CAST, IS NULL, BETWEEN, IN (value list).
//
// A column reference is resolved to its position via Schema::indexOf, matching
// both the unqualified name and the "alias.name" qualified form the planner
// produces. Aggregate function calls are NOT evaluated here — the aggregate
// operator handles them; a stray aggregate reaching Evaluator is an error.
// ============================================================================

#include "ast.h"
#include "types.h"

#include <string>

namespace lq {

struct EvalError : std::runtime_error {
    explicit EvalError(const std::string& msg) : std::runtime_error(msg) {}
};

// Evaluate `expr` for a single input row `row` whose columns are described by
// `schema`. Returns the resulting Value (possibly NULL).
Value evaluate(const ast::ExprNode& expr, const Schema& schema, const Row& row);

// Convenience: evaluate a predicate to a definite boolean. A NULL or non-bool
// predicate result counts as false (SQL WHERE semantics).
bool evaluatePredicate(const ast::ExprNode& expr, const Schema& schema, const Row& row);

// SQL LIKE matcher (`%` = any run, `_` = one char). caseInsensitive → ILIKE.
bool likeMatch(const std::string& text, const std::string& pattern, bool caseInsensitive);

}  // namespace lq
