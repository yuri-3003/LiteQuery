#pragma once

// ============================================================================
// LiteQuery — logical_plan.h
// Relational-algebra tree: the logical plan produced from the AST.
//
// The logical plan is a tree of typed nodes (LogicalScan, LogicalFilter, …)
// held in a std::variant (LogicalNode). Each node carries:
//   - its output Schema (so every parent can resolve column references), and
//   - a NodeId (a stable identifier used by EXPLAIN and the optimizer stats).
//
// A logical plan describes *what* to compute (relational algebra) but not
// *how* — physical operator selection, join algorithms, and vectorization
// happen later in physical_plan. The optimizer rewrites this tree in place.
//
// Ownership: children are held by LogicalPlan = unique_ptr<LogicalNode>, so
// the tree has single ownership and destroys recursively. Expressions embedded
// in nodes (filter predicates, projection items) are ast::Expr (unique_ptr).
// ============================================================================

#include "ast.h"       // ast::Expr, ast::ExprNode, ast::SortOrder, ast::JoinType, …
#include "types.h"     // Schema, DataType, Value

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace lq {

class Catalog;   // catalog.h

namespace plan {

// ============================================================================
// PlanError — raised during logical planning (unknown table/column, etc.)
// ============================================================================

struct PlanError : std::runtime_error {
    explicit PlanError(const std::string& msg) : std::runtime_error(msg) {}
};

// ============================================================================
// NodeId — stable per-node identifier
// ============================================================================

using NodeId = uint32_t;

// ============================================================================
// Forward declaration of the node variant and its handle
// ============================================================================

struct LogicalScan;
struct LogicalFilter;
struct LogicalProject;
struct LogicalAggregate;
struct LogicalSort;
struct LogicalLimit;
struct LogicalJoin;
struct LogicalUnion;
struct LogicalDistinct;
struct LogicalValues;

using LogicalNode = std::variant<
    LogicalScan,
    LogicalFilter,
    LogicalProject,
    LogicalAggregate,
    LogicalSort,
    LogicalLimit,
    LogicalJoin,
    LogicalUnion,
    LogicalDistinct,
    LogicalValues
>;

using LogicalPlan = std::unique_ptr<LogicalNode>;

// ============================================================================
// Supporting value types embedded in nodes
// ============================================================================

// One output column of a LogicalProject: an expression + its result name/type.
struct ProjectItem {
    ast::Expr   expr;
    std::string outputName;
    DataType    outputType;
};

// One aggregate function call inside a LogicalAggregate.
struct AggCall {
    std::string   funcName;         // "SUM", "COUNT", "AVG", …  (upper-cased)
    ast::ExprList args;             // Argument expressions (empty for COUNT(*))
    bool          distinct = false; // COUNT(DISTINCT x)
    bool          star     = false; // COUNT(*)
    std::string   outputName;       // Result column name
    DataType      outputType;       // Result column type
};

// One ORDER BY key inside a LogicalSort.
struct SortSpec {
    ast::Expr        expr;
    ast::SortOrder   order      = ast::SortOrder::ASC;
    ast::NullsOrder  nullsOrder = ast::NullsOrder::UNSPECIFIED;
};

// ============================================================================
// Logical node structs
//
// Every node has `outputSchema` and `nodeId` as its LAST two members so the
// generic free-function accessors below can read them uniformly via std::visit.
// ============================================================================

// Leaf: read a base table. The optimizer may fill columnPruning / pushedPredicate.
//
// Field order note: the planner constructs this node positionally as
//   LogicalScan{schemaName, tableName, alias, outputSchema, nodeId}
// so `outputSchema`/`nodeId` come before the optimizer-only annotations, which
// are always left default-constructed at planning time and filled in later.
struct LogicalScan {
    std::string           schemaName;       // Optional schema qualifier ("" if none)
    std::string           tableName;
    std::string           alias;            // "" if unaliased

    Schema  outputSchema;
    NodeId  nodeId = 0;

    std::vector<size_t>   columnPruning;    // Column indices to read; empty = all
    ast::Expr             pushedPredicate;  // Predicate fused into the scan (or null)
};

// σ — keep rows satisfying `predicate`.
struct LogicalFilter {
    LogicalPlan child;
    ast::Expr   predicate;

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// π — compute a list of output expressions.
struct LogicalProject {
    LogicalPlan               child;
    std::vector<ProjectItem>  items;

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// Γ — GROUP BY + aggregate.
struct LogicalAggregate {
    LogicalPlan            child;
    ast::ExprList          groupKeys;
    std::vector<AggCall>   aggCalls;

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// ORDER BY.
struct LogicalSort {
    LogicalPlan            child;
    std::vector<SortSpec>  keys;

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// LIMIT / OFFSET.
struct LogicalLimit {
    LogicalPlan             child;
    std::optional<int64_t>  limit;
    std::optional<int64_t>  offset;

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// ⋈ — join two subplans.
struct LogicalJoin {
    ast::JoinType            type = ast::JoinType::INNER;
    LogicalPlan              left;
    LogicalPlan              right;
    std::optional<ast::Expr> condition;   // nullopt for CROSS JOIN

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// ∪ — UNION [ALL].
struct LogicalUnion {
    LogicalPlan left;
    LogicalPlan right;
    bool        all = false;   // true = UNION ALL (keep duplicates)

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// DISTINCT.
struct LogicalDistinct {
    LogicalPlan child;

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// Inline constant relation: VALUES (…), (…) — also the "dual" 1-row source.
struct LogicalValues {
    std::vector<std::vector<ast::Expr>> rows;

    Schema  outputSchema;
    NodeId  nodeId = 0;
};

// ============================================================================
// Generic accessors — read the common trailing members from any node
// ============================================================================

const Schema& outputSchema(const LogicalNode& node) noexcept;
Schema&       outputSchema(LogicalNode& node) noexcept;
NodeId        nodeId(const LogicalNode& node) noexcept;

// Heap-allocate a node of type T and wrap it as a LogicalPlan.
template <typename T, typename... Args>
LogicalPlan makeNode(Args&&... args) {
    return std::make_unique<LogicalNode>(T{std::forward<Args>(args)...});
}

// ============================================================================
// PlanPrinter — render a logical plan as an indented tree (EXPLAIN output)
// ============================================================================

class PlanPrinter {
public:
    explicit PlanPrinter(int indent = 0) : indent_(indent) {}

    // Render an entire plan.
    std::string print(const LogicalNode& node) {
        return std::visit([this](const auto& n) { return visit(n); }, node);
    }

    std::string visit(const LogicalScan& n);
    std::string visit(const LogicalFilter& n);
    std::string visit(const LogicalProject& n);
    std::string visit(const LogicalAggregate& n);
    std::string visit(const LogicalSort& n);
    std::string visit(const LogicalLimit& n);
    std::string visit(const LogicalJoin& n);
    std::string visit(const LogicalUnion& n);
    std::string visit(const LogicalDistinct& n);
    std::string visit(const LogicalValues& n);

private:
    std::string schemaStr(const Schema& s) const;
    std::string ind() const { return std::string(indent_ * indentWidth_, ' '); }

    std::string child(const LogicalNode& n) {
        ++indent_;
        std::string s = std::visit([this](const auto& c) { return visit(c); }, n);
        --indent_;
        return s;
    }

    int indent_      = 0;
    int indentWidth_ = 2;
};

// ============================================================================
// LogicalPlanner — AST → logical plan
//
// One planner instance per statement. Resolves column references against the
// catalog, builds the relational-algebra tree, and validates basic semantics
// (unknown table/column, LIMIT must be a non-negative integer, …).
// ============================================================================

class LogicalPlanner {
public:
    explicit LogicalPlanner(const Catalog& catalog);

    LogicalPlan plan(const ast::SelectStmt& stmt);
    LogicalPlan plan(const ast::InsertStmt& stmt);

private:
    // Clause planners — each wraps `input` with the next relational operator.
    LogicalPlan planFrom(const ast::FromClause& from);
    LogicalPlan planTableRef(const ast::TableRefNode& ref);
    LogicalPlan planJoin(const ast::JoinRef& join);
    LogicalPlan planWhere(const ast::WhereClause& where, LogicalPlan input);
    LogicalPlan planGroupBy(const ast::SelectStmt& stmt, LogicalPlan input);
    LogicalPlan planProjection(const ast::SelectStmt& stmt, LogicalPlan input);
    LogicalPlan planOrderBy(const ast::OrderByClause& ob, LogicalPlan input);
    LogicalPlan planLimit(const ast::LimitClause& lim, LogicalPlan input);
    LogicalPlan planSetOp(const ast::SelectStmt::SetOpTail& tail, LogicalPlan left);

    // Semantic helpers.
    void resolveExpr(const ast::ExprNode& expr, const Schema& scope) const;

    std::vector<ProjectItem> expandSelectList(
        const std::vector<ast::SelectItem>& items, const Schema& inputSchema);

    static bool containsAggregates(const ast::ExprNode& expr);
    static bool anyContainsAggregates(const std::vector<ast::SelectItem>& items);

    std::vector<AggCall> collectAggCalls(
        const std::vector<ast::SelectItem>& selectList,
        const std::optional<ast::HavingClause>& having);

    static int64_t resolveIntConstant(const ast::ExprNode& expr,
                                      std::string_view context);

    NodeId nextId() { return ++idCounter_; }

    const Catalog& catalog_;
    NodeId         idCounter_ = 0;
};

}  // namespace plan
}  // namespace lq
