// ============================================================================
// LiteQuery — logical_plan.cpp
// Schema accessors, PlanPrinter, and LogicalPlanner implementation.
// ============================================================================

#include "logical_plan.h"
#include "catalog.h"   // see catalog.h for Catalog / TableStats declarations

#include <algorithm>
#include <cassert>
#include <sstream>
#include <stdexcept>

namespace lq {
namespace plan {

// ============================================================================
// Free-function schema / nodeId accessors
// ============================================================================

const Schema& outputSchema(const LogicalNode& node) noexcept {
    return std::visit([](const auto& n) -> const Schema& {
        return n.outputSchema;
    }, node);
}

Schema& outputSchema(LogicalNode& node) noexcept {
    return std::visit([](auto& n) -> Schema& {
        return n.outputSchema;
    }, node);
}

NodeId nodeId(const LogicalNode& node) noexcept {
    return std::visit([](const auto& n) -> NodeId {
        return n.nodeId;
    }, node);
}

// ============================================================================
// PlanPrinter
// ============================================================================

std::string PlanPrinter::schemaStr(const Schema& s) const {
    std::string out = "[";
    for (size_t i = 0; i < s.size(); ++i) {
        if (i) out += ", ";
        out += s[i].name + ":" + s[i].type.toString();
    }
    return out + "]";
}

std::string PlanPrinter::visit(const LogicalScan& n) {
    std::string s = ind() + "Scan(" + n.tableName;
    if (!n.alias.empty()) s += " AS " + n.alias;
    s += ") → " + schemaStr(n.outputSchema);
    if (!n.columnPruning.empty()) {
        s += " [pruned cols:";
        for (auto c : n.columnPruning) s += " " + std::to_string(c);
        s += "]";
    }
    if (n.pushedPredicate)
        s += "\n" + std::string(indent_ * indentWidth_ + 2, ' ') + "pushdown: <predicate>";
    return s;
}

std::string PlanPrinter::visit(const LogicalFilter& n) {
    return ind() + "Filter\n" + child(*n.child);
}

std::string PlanPrinter::visit(const LogicalProject& n) {
    std::string s = ind() + "Project(";
    for (size_t i = 0; i < n.items.size(); ++i) {
        if (i) s += ", ";
        s += n.items[i].outputName;
    }
    s += ")\n" + child(*n.child);
    return s;
}

std::string PlanPrinter::visit(const LogicalAggregate& n) {
    std::string s = ind() + "Aggregate(";
    s += "groups=" + std::to_string(n.groupKeys.size());
    s += ", aggs=" + std::to_string(n.aggCalls.size()) + ")\n";
    // Print agg calls
    for (const auto& agg : n.aggCalls) {
        s += std::string((indent_ + 1) * indentWidth_, ' ');
        s += agg.funcName;
        if (agg.distinct) s += "[DISTINCT]";
        if (agg.star)     s += "(*)";
        s += " → " + agg.outputName + "\n";
    }
    s += child(*n.child);
    return s;
}

std::string PlanPrinter::visit(const LogicalSort& n) {
    std::string s = ind() + "Sort(";
    for (size_t i = 0; i < n.keys.size(); ++i) {
        if (i) s += ", ";
        s += (n.keys[i].order == ast::SortOrder::ASC ? "ASC" : "DESC");
    }
    s += ")\n" + child(*n.child);
    return s;
}

std::string PlanPrinter::visit(const LogicalLimit& n) {
    std::string s = ind() + "Limit(";
    if (n.limit)  s += "limit="  + std::to_string(*n.limit);
    if (n.offset) s += " offset=" + std::to_string(*n.offset);
    s += ")\n" + child(*n.child);
    return s;
}

std::string PlanPrinter::visit(const LogicalJoin& n) {
    std::string s = ind() + "Join(";
    switch (n.type) {
        case ast::JoinType::INNER: s += "INNER"; break;
        case ast::JoinType::LEFT:  s += "LEFT";  break;
        case ast::JoinType::RIGHT: s += "RIGHT"; break;
        case ast::JoinType::FULL:  s += "FULL";  break;
        case ast::JoinType::CROSS: s += "CROSS"; break;
    }
    s += ")\n";
    s += child(*n.left)  + "\n";
    s += child(*n.right);
    return s;
}

std::string PlanPrinter::visit(const LogicalUnion& n) {
    std::string s = ind() + std::string(n.all ? "UnionAll\n" : "Union\n");
    s += child(*n.left)  + "\n";
    s += child(*n.right);
    return s;
}

std::string PlanPrinter::visit(const LogicalDistinct& n) {
    return ind() + "Distinct\n" + child(*n.child);
}

std::string PlanPrinter::visit(const LogicalValues& n) {
    return ind() + "Values(" + std::to_string(n.rows.size()) + " rows)";
}

// ============================================================================
// LogicalPlanner
// ============================================================================

LogicalPlanner::LogicalPlanner(const Catalog& catalog)
    : catalog_(catalog) {}

// ---- Entry point -----------------------------------------------------------

LogicalPlan LogicalPlanner::plan(const ast::SelectStmt& stmt) {
    // Step 1: Build the FROM clause tree (scan + joins)
    LogicalPlan input = stmt.from
        ? planFrom(*stmt.from)
        : makeNode<LogicalValues>(
              std::vector<std::vector<ast::Expr>>{},
              Schema{}.addColumn("__dual__", DataType::int32()),
              nextId());

    // Step 2: WHERE filter
    if (stmt.where)
        input = planWhere(*stmt.where, std::move(input));

    // Step 3: GROUP BY / aggregation (or plain projection)
    // Step 4: HAVING (folded into planGroupBy as a post-aggregate filter)
    // Step 5: DISTINCT / projection
    input = planGroupBy(stmt, std::move(input));

    // Step 6: ORDER BY
    if (stmt.orderBy)
        input = planOrderBy(*stmt.orderBy, std::move(input));

    // Step 7: LIMIT / OFFSET
    if (stmt.limit)
        input = planLimit(*stmt.limit, std::move(input));

    // Step 8: UNION / INTERSECT / EXCEPT
    if (stmt.setOp)
        input = planSetOp(*stmt.setOp, std::move(input));

    return input;
}

LogicalPlan LogicalPlanner::plan(const ast::InsertStmt& stmt) {
    // Resolve the target table schema from the catalog
    const Schema& tableSchema = catalog_.getSchema(stmt.table);
    (void)tableSchema;  // Used for type checking (future)

    return std::visit([&](const auto& src) -> LogicalPlan {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, std::vector<ast::ExprList>>) {
            // INSERT INTO t VALUES (r1), (r2), ...
            std::vector<std::vector<ast::Expr>> rows;
            for (const auto& row : src) {
                std::vector<ast::Expr> rowExprs;
                for (const auto& e : row) {
                    // Deep-copy the expression (simplified: move from source)
                    // In production, expressions are cloned properly.
                    (void)e;
                }
                rows.push_back(std::move(rowExprs));
            }
            return makeNode<LogicalValues>(std::move(rows), tableSchema, nextId());
        } else {
            // INSERT INTO t SELECT ...
            return plan(*src);
        }
    }, stmt.source);
}

// ---- FROM clause -----------------------------------------------------------

LogicalPlan LogicalPlanner::planFrom(const ast::FromClause& from) {
    if (from.tables.empty())
        throw PlanError("FROM clause has no tables");

    // Build a left-deep join tree for comma-separated tables
    LogicalPlan result = planTableRef(*from.tables[0]);
    for (size_t i = 1; i < from.tables.size(); ++i) {
        auto right = planTableRef(*from.tables[i]);
        // Comma-separated tables → implicit CROSS JOIN
        Schema merged = Schema::merge(outputSchema(*result), outputSchema(*right));
        result = makeNode<LogicalJoin>(
            ast::JoinType::CROSS,
            std::move(result),
            std::move(right),
            std::nullopt,
            merged,
            nextId());
    }
    return result;
}

LogicalPlan LogicalPlanner::planTableRef(const ast::TableRefNode& ref) {
    return std::visit([this](const auto& r) -> LogicalPlan {
        using T = std::decay_t<decltype(r)>;

        if constexpr (std::is_same_v<T, ast::TableRef>) {
            // Look up the table in the catalog
            const Schema& tableSchema = catalog_.getSchema(r.name);
            Schema outputSch = tableSchema;
            return makeNode<LogicalScan>(
                r.schema.value_or(""),
                r.name,
                /*alias=*/"",
                outputSch,
                nextId());
        }

        else if constexpr (std::is_same_v<T, ast::AliasedRef>) {
            // Plan the inner ref, then rename its output to use the alias
            LogicalPlan inner = planTableRef(*r.ref);
            const Schema& innerSchema = outputSchema(*inner);

            // Prefix all column names with the alias for qualified name resolution
            Schema aliasedSchema;
            for (size_t i = 0; i < innerSchema.size(); ++i) {
                aliasedSchema.addColumn(
                    r.alias + "." + innerSchema[i].name,
                    innerSchema[i].type);
            }

            // Wrap in a trivial project that just renames (optimizer will elide it)
            std::vector<ProjectItem> items;
            items.reserve(innerSchema.size());
            for (size_t i = 0; i < innerSchema.size(); ++i) {
                ast::ColumnRef cref;
                cref.column = innerSchema[i].name;
                items.push_back({
                    std::make_unique<ast::ExprNode>(cref),
                    r.alias + "." + innerSchema[i].name,
                    innerSchema[i].type
                });
            }
            return makeNode<LogicalProject>(std::move(inner), std::move(items),
                                            aliasedSchema, nextId());
        }

        else if constexpr (std::is_same_v<T, ast::JoinRef>) {
            return planJoin(r);
        }

        else if constexpr (std::is_same_v<T, ast::SubqueryRef>) {
            if (!r.subquery)
                throw PlanError("NULL subquery reference");
            return plan(*r.subquery);
        }

        throw PlanError("Unknown table reference type");
    }, ref);
}

LogicalPlan LogicalPlanner::planJoin(const ast::JoinRef& join) {
    auto left  = planTableRef(*join.left);
    auto right = planTableRef(*join.right);

    Schema merged = Schema::merge(outputSchema(*left), outputSchema(*right));

    // Validate that ON condition columns exist in the merged schema (simplified)
    if (join.condition)
        resolveExpr(**join.condition, merged);

    // Clone the condition expression — in production, this would deep-copy the AST.
    // For now we leave condition as nullopt in the plan node and rely on
    // the optimizer to re-attach it (the condition is still in the AST).
    std::optional<ast::Expr> cond = std::nullopt;
    (void)cond;  // Will be addressed in the expression cloning utility

    return makeNode<LogicalJoin>(join.type,
                                  std::move(left),
                                  std::move(right),
                                  std::nullopt,   // condition attached by optimizer
                                  merged,
                                  nextId());
}

// ---- WHERE -----------------------------------------------------------------

LogicalPlan LogicalPlanner::planWhere(const ast::WhereClause& where,
                                       LogicalPlan input) {
    const Schema& inputSch = outputSchema(*input);
    resolveExpr(*where.predicate, inputSch);

    Schema outSch = inputSch;  // Filter does not change schema

    // Expression ownership: in a real planner we'd clone the AST expression.
    // Here we build a placeholder ColumnRef as the predicate node.
    ast::ColumnRef placeholder;
    placeholder.column = "__where__";

    return makeNode<LogicalFilter>(
        std::move(input),
        std::make_unique<ast::ExprNode>(placeholder),
        outSch,
        nextId());
}

// ---- GROUP BY / HAVING / projection ----------------------------------------

LogicalPlan LogicalPlanner::planGroupBy(const ast::SelectStmt& stmt,
                                         LogicalPlan input) {
    const bool hasAgg = anyContainsAggregates(stmt.selectList)
                     || stmt.groupBy.has_value()
                     || stmt.having.has_value();

    if (!hasAgg) {
        // Simple projection (no aggregation)
        return planProjection(stmt, std::move(input));
    }

    const Schema& inputSch = outputSchema(*input);

    // Build group keys
    ast::ExprList groupKeys;
    if (stmt.groupBy) {
        for (const auto& key : stmt.groupBy->keys) {
            resolveExpr(*key, inputSch);
            ast::ColumnRef ref;
            ref.column = "__groupkey__";
            groupKeys.push_back(std::make_unique<ast::ExprNode>(ref));
        }
    }

    // Collect aggregate calls from SELECT list + HAVING
    std::vector<AggCall> aggCalls = collectAggCalls(stmt.selectList, stmt.having);

    // Build aggregate output schema:
    //   [group key columns] ++ [aggregate result columns]
    Schema aggOutputSch;
    if (stmt.groupBy) {
        for (const auto& key : stmt.groupBy->keys) {
            // In a full implementation we'd look up the key type from inputSch
            aggOutputSch.addColumn("__key__", DataType::int64());
        }
    }
    for (const auto& agg : aggCalls) {
        aggOutputSch.addColumn(agg.outputName, agg.outputType);
    }

    auto aggNode = makeNode<LogicalAggregate>(
        std::move(input),
        std::move(groupKeys),
        std::move(aggCalls),
        aggOutputSch,
        nextId());

    // HAVING → LogicalFilter on top of aggregate
    if (stmt.having) {
        const Schema& aggSch = outputSchema(*aggNode);
        resolveExpr(*stmt.having->predicate, aggSch);
        ast::ColumnRef havingRef;
        havingRef.column = "__having__";
        aggNode = makeNode<LogicalFilter>(
            std::move(aggNode),
            std::make_unique<ast::ExprNode>(havingRef),
            aggSch,
            nextId());
    }

    // Final projection on top of aggregate to apply aliases and computed cols
    return planProjection(stmt, std::move(aggNode));
}

LogicalPlan LogicalPlanner::planProjection(const ast::SelectStmt& stmt,
                                             LogicalPlan input) {
    const Schema& inputSch = outputSchema(*input);
    auto items = expandSelectList(stmt.selectList, inputSch);

    // Build output schema from expanded items
    Schema outSch;
    for (const auto& item : items)
        outSch.addColumn(item.outputName, item.outputType);

    auto proj = makeNode<LogicalProject>(
        std::move(input), std::move(items), outSch, nextId());

    // SELECT DISTINCT → wrap in LogicalDistinct
    if (stmt.distinct) {
        Schema distinctSch = outputSchema(*proj);
        proj = makeNode<LogicalDistinct>(std::move(proj), distinctSch, nextId());
    }

    return proj;
}

// ---- ORDER BY / LIMIT ------------------------------------------------------

LogicalPlan LogicalPlanner::planOrderBy(const ast::OrderByClause& ob,
                                          LogicalPlan input) {
    const Schema& inputSch = outputSchema(*input);
    std::vector<SortSpec> specs;
    for (const auto& key : ob.keys) {
        resolveExpr(*key.expr, inputSch);
        ast::ColumnRef sortRef;
        sortRef.column = "__sortkey__";
        specs.push_back({
            std::make_unique<ast::ExprNode>(sortRef),
            key.order,
            key.nullsOrder
        });
    }
    Schema outSch = inputSch;
    return makeNode<LogicalSort>(std::move(input), std::move(specs), outSch, nextId());
}

LogicalPlan LogicalPlanner::planLimit(const ast::LimitClause& lim,
                                        LogicalPlan input) {
    std::optional<int64_t> limitVal;
    std::optional<int64_t> offsetVal;

    if (lim.limit) {
        limitVal = resolveIntConstant(**lim.limit, "LIMIT");
        if (*limitVal < 0)
            throw PlanError("LIMIT must be non-negative");
    }
    if (lim.offset) {
        offsetVal = resolveIntConstant(**lim.offset, "OFFSET");
        if (*offsetVal < 0)
            throw PlanError("OFFSET must be non-negative");
    }

    Schema outSch = outputSchema(*input);
    return makeNode<LogicalLimit>(std::move(input), limitVal, offsetVal,
                                   outSch, nextId());
}

// ---- SET operations --------------------------------------------------------

LogicalPlan LogicalPlanner::planSetOp(const ast::SelectStmt::SetOpTail& tail,
                                        LogicalPlan left) {
    if (!tail.rhs)
        throw PlanError("UNION/INTERSECT/EXCEPT requires a right-hand query");

    auto right = plan(*tail.rhs);

    // Validate schema compatibility
    const Schema& lSch = outputSchema(*left);
    const Schema& rSch = outputSchema(*right);
    if (lSch.size() != rSch.size())
        throw PlanError("UNION requires both queries to have the same number of columns");

    // Use left schema for output (right schema's types must be compatible)
    Schema outSch = lSch;

    if (tail.op == ast::SetOp::UNION || tail.op == ast::SetOp::UNION_ALL) {
        return makeNode<LogicalUnion>(
            std::move(left), std::move(right),
            tail.op == ast::SetOp::UNION_ALL,
            outSch, nextId());
    }

    // INTERSECT / EXCEPT — handled as distinct union variants by the optimizer.
    // For now raise a not-yet-implemented error.
    throw PlanError("INTERSECT and EXCEPT are not yet implemented");
}

// ---- Expression resolution -------------------------------------------------

void LogicalPlanner::resolveExpr(const ast::ExprNode& expr,
                                   const Schema& scope) const {
    std::visit([&](const auto& e) {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, ast::ColumnRef>) {
            if (e.isStar()) return;   // SELECT * — resolved later
            // Unqualified name: look it up directly
            if (!e.table.has_value()) {
                if (scope.indexOf(e.column) < 0)
                    throw PlanError("Unknown column: " + e.column);
                return;
            }
            // Qualified name: look up "table.column"
            std::string qualified = *e.table + "." + e.column;
            if (scope.indexOf(qualified) >= 0) return;
            // Also accept unqualified match
            if (scope.indexOf(e.column) >= 0) return;
            throw PlanError("Unknown column: " + e.toString());
        }

        else if constexpr (std::is_same_v<T, ast::BinaryExpr>) {
            resolveExpr(*e.left, scope);
            resolveExpr(*e.right, scope);
        }

        else if constexpr (std::is_same_v<T, ast::UnaryExpr>) {
            resolveExpr(*e.operand, scope);
        }

        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            for (const auto& arg : e.args)
                resolveExpr(*arg, scope);
        }

        else if constexpr (std::is_same_v<T, ast::CastExpr>) {
            resolveExpr(*e.expr, scope);
        }

        else if constexpr (std::is_same_v<T, ast::IsNullExpr>) {
            resolveExpr(*e.expr, scope);
        }

        else if constexpr (std::is_same_v<T, ast::BetweenExpr>) {
            resolveExpr(*e.expr, scope);
            resolveExpr(*e.low, scope);
            resolveExpr(*e.high, scope);
        }

        else if constexpr (std::is_same_v<T, ast::CaseExpr>) {
            if (e.subject) resolveExpr(**e.subject, scope);
            for (const auto& w : e.whenClauses) {
                resolveExpr(*w.condition, scope);
                resolveExpr(*w.result, scope);
            }
            if (e.elseExpr) resolveExpr(**e.elseExpr, scope);
        }

        // Literals, SubqueryExpr, InExpr etc. — omitted for brevity;
        // a complete implementation handles all 13 ExprNode alternatives.

    }, expr);
}

// ---- SELECT list expansion -------------------------------------------------

std::vector<ProjectItem> LogicalPlanner::expandSelectList(
    const std::vector<ast::SelectItem>& items,
    const Schema& inputSchema)
{
    std::vector<ProjectItem> result;

    for (const auto& item : items) {
        // Check for SELECT *
        const auto* colRef = std::get_if<ast::ColumnRef>(item.expr.get());
        if (colRef && colRef->isStar()) {
            // Expand all columns from input schema (or table.* qualified)
            for (size_t i = 0; i < inputSchema.size(); ++i) {
                const auto& col = inputSchema[i];
                if (colRef->table && col.name.rfind(*colRef->table + ".") != 0)
                    continue;  // Filter to specific table's columns
                ast::ColumnRef ref;
                ref.column = col.name;
                result.push_back({
                    std::make_unique<ast::ExprNode>(ref),
                    col.name,
                    col.type
                });
            }
            continue;
        }

        // Regular expression item
        std::string name = item.alias.value_or("");
        if (name.empty()) {
            // Synthesise a column name from the expression
            if (colRef) {
                name = colRef->column;
            } else {
                name = "col" + std::to_string(result.size() + 1);
            }
        }

        // Type resolution placeholder — in a full impl the TypeChecker pass
        // walks the expression and returns the resolved DataType.
        DataType type = DataType::float64();

        ast::ColumnRef placeholder;
        placeholder.column = name;
        result.push_back({
            std::make_unique<ast::ExprNode>(placeholder),
            name,
            type
        });
    }

    return result;
}

// ---- Aggregate detection ---------------------------------------------------

bool LogicalPlanner::containsAggregates(const ast::ExprNode& expr) {
    return std::visit([](const auto& e) -> bool {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ast::FunctionCall>)
            return e.isAggregate();
        if constexpr (std::is_same_v<T, ast::BinaryExpr>)
            return containsAggregates(*e.left) || containsAggregates(*e.right);
        if constexpr (std::is_same_v<T, ast::UnaryExpr>)
            return containsAggregates(*e.operand);
        return false;
    }, expr);
}

bool LogicalPlanner::anyContainsAggregates(
    const std::vector<ast::SelectItem>& items)
{
    for (const auto& item : items)
        if (containsAggregates(*item.expr)) return true;
    return false;
}

std::vector<AggCall> LogicalPlanner::collectAggCalls(
    const std::vector<ast::SelectItem>& selectList,
    const std::optional<ast::HavingClause>& having)
{
    std::vector<AggCall> calls;

    auto extract = [&](const ast::ExprNode& expr, std::string_view hint) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                if (e.isAggregate()) {
                    AggCall agg;
                    agg.funcName   = e.name;
                    agg.distinct   = e.distinct;
                    agg.star       = e.star;
                    agg.outputName = std::string(hint.empty() ? e.name : hint);
                    // Type resolution would happen here via TypeChecker
                    agg.outputType = DataType::float64();
                    calls.push_back(std::move(agg));
                }
            }
        }, expr);
    };

    for (const auto& item : selectList)
        extract(*item.expr, item.alias.value_or(""));

    if (having)
        extract(*having->predicate, "");

    return calls;
}

// ---- Constant resolution ---------------------------------------------------

int64_t LogicalPlanner::resolveIntConstant(const ast::ExprNode& expr,
                                             std::string_view context) {
    const auto* lit = std::get_if<ast::Literal>(&expr);
    if (!lit)
        throw PlanError(std::string(context) + " must be an integer constant");

    try {
        return lit->value.toInt64();
    } catch (...) {
        throw PlanError(std::string(context) + " expression is not an integer");
    }
}

}  // namespace plan
}  // namespace lq
