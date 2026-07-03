// ============================================================================
// LiteQuery — optimizer.cpp
// Rule-based logical query optimizer implementation
// ============================================================================

#include "optimizer.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace lq {
namespace opt  {

using namespace plan;
using plan::outputSchema;
using plan::LogicalScan;
using plan::LogicalFilter;
using plan::LogicalProject;
using plan::LogicalAggregate;
using plan::LogicalSort;
using plan::LogicalLimit;
using plan::LogicalJoin;
using plan::LogicalUnion;
using plan::LogicalDistinct;
using plan::LogicalValues;
using namespace ast;

// ============================================================================
// ExprUtils
// ============================================================================

namespace ExprUtils {

bool isConstant(const ExprNode& expr) noexcept {
    return std::visit([](const auto& e) -> bool {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, Literal>)     return true;
        if constexpr (std::is_same_v<T, ColumnRef>)   return false;
        if constexpr (std::is_same_v<T, FunctionCall>) return false; // may have side-effects
        if constexpr (std::is_same_v<T, BinaryExpr>)
            return isConstant(*e.left) && isConstant(*e.right);
        if constexpr (std::is_same_v<T, UnaryExpr>)
            return isConstant(*e.operand);
        if constexpr (std::is_same_v<T, CastExpr>)
            return isConstant(*e.expr);
        return false;
    }, expr);
}

Value evalConstant(const ExprNode& expr) {
    return std::visit([](const auto& e) -> Value {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, Literal>)
            return e.value;

        if constexpr (std::is_same_v<T, UnaryExpr>) {
            Value v = evalConstant(*e.operand);
            if (v.isNull()) return Value::null();
            if (e.op == UnaryOp::NEGATE) {
                if (v.typeId() == TypeId::INT64)  return Value(-v.getInt64());
                if (v.typeId() == TypeId::FLOAT64) return Value(-v.getDouble());
            }
            if (e.op == UnaryOp::NOT) {
                if (v.typeId() == TypeId::BOOLEAN) return Value(!v.getBool());
            }
            return Value::null();
        }

        if constexpr (std::is_same_v<T, BinaryExpr>) {
            Value l = evalConstant(*e.left);
            Value r = evalConstant(*e.right);

            // NULL propagation
            if (l.isNull() || r.isNull()) return Value::null();

            // Arithmetic — promote to double for simplicity
            if (e.op == BinaryOp::ADD || e.op == BinaryOp::SUB ||
                e.op == BinaryOp::MUL || e.op == BinaryOp::DIV) {
                try {
                    double lv = l.toDouble(), rv = r.toDouble();
                    switch (e.op) {
                        case BinaryOp::ADD: return Value(lv + rv);
                        case BinaryOp::SUB: return Value(lv - rv);
                        case BinaryOp::MUL: return Value(lv * rv);
                        case BinaryOp::DIV:
                            if (rv == 0.0) return Value::null(); // div-by-zero → NULL
                            return Value(lv / rv);
                        default: break;
                    }
                } catch (...) { return Value::null(); }
            }

            // Comparisons
            switch (e.op) {
                case BinaryOp::EQ:  return Value(l == r);
                case BinaryOp::NEQ: return Value(l != r);
                case BinaryOp::LT:  return Value(l <  r);
                case BinaryOp::LTE: return Value(l <= r);
                case BinaryOp::GT:  return Value(l >  r);
                case BinaryOp::GTE: return Value(l >= r);
                default: break;
            }

            // Logical
            if (e.op == BinaryOp::AND) {
                if (l.typeId() == TypeId::BOOLEAN && r.typeId() == TypeId::BOOLEAN)
                    return Value(l.getBool() && r.getBool());
            }
            if (e.op == BinaryOp::OR) {
                if (l.typeId() == TypeId::BOOLEAN && r.typeId() == TypeId::BOOLEAN)
                    return Value(l.getBool() || r.getBool());
            }

            // String concat
            if (e.op == BinaryOp::CONCAT) {
                if (l.typeId() == TypeId::VARCHAR && r.typeId() == TypeId::VARCHAR)
                    return Value(l.getString() + r.getString());
            }

            return Value::null();
        }

        if constexpr (std::is_same_v<T, CastExpr>) {
            Value v = evalConstant(*e.expr);
            if (v.isNull()) return Value::null();
            // Simplified: only int→double and double→int casts
            if (e.targetType.id == TypeId::FLOAT64 && typeIsInteger(v.typeId()))
                return Value(static_cast<double>(v.toInt64()));
            if (typeIsInteger(e.targetType.id) && v.typeId() == TypeId::FLOAT64)
                return Value(static_cast<int64_t>(v.getDouble()));
            return v; // identity cast
        }

        return Value::null();
    }, expr);
}

bool isAlwaysTrue(const ExprNode& expr) noexcept {
    const auto* lit = std::get_if<Literal>(&expr);
    if (!lit) return false;
    if (lit->value.typeId() == TypeId::BOOLEAN) return lit->value.getBool();
    return false;
}

bool isAlwaysFalse(const ExprNode& expr) noexcept {
    const auto* lit = std::get_if<Literal>(&expr);
    if (!lit) return false;
    if (lit->value.isNull()) return true;
    if (lit->value.typeId() == TypeId::BOOLEAN) return !lit->value.getBool();
    return false;
}

void collectColumns(const ExprNode& expr, ColumnSet& out) {
    std::visit([&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ColumnRef>) {
            if (!e.isStar()) {
                // Store both qualified and unqualified form
                out.insert(e.column);
                if (e.table) out.insert(*e.table + "." + e.column);
            }
        }
        if constexpr (std::is_same_v<T, BinaryExpr>) {
            collectColumns(*e.left, out);
            collectColumns(*e.right, out);
        }
        if constexpr (std::is_same_v<T, UnaryExpr>)
            collectColumns(*e.operand, out);
        if constexpr (std::is_same_v<T, FunctionCall>)
            for (const auto& a : e.args) collectColumns(*a, out);
        if constexpr (std::is_same_v<T, CastExpr>)
            collectColumns(*e.expr, out);
        if constexpr (std::is_same_v<T, IsNullExpr>)
            collectColumns(*e.expr, out);
        if constexpr (std::is_same_v<T, BetweenExpr>) {
            collectColumns(*e.expr, out);
            collectColumns(*e.low, out);
            collectColumns(*e.high, out);
        }
        if constexpr (std::is_same_v<T, CaseExpr>) {
            if (e.subject) collectColumns(**e.subject, out);
            for (const auto& w : e.whenClauses) {
                collectColumns(*w.condition, out);
                collectColumns(*w.result, out);
            }
            if (e.elseExpr) collectColumns(**e.elseExpr, out);
        }
    }, expr);
}

static void splitConj(const ExprNode& expr, std::vector<const ExprNode*>& out) {
    const auto* bin = std::get_if<BinaryExpr>(&expr);
    if (bin && bin->op == BinaryOp::AND) {
        splitConj(*bin->left,  out);
        splitConj(*bin->right, out);
    } else {
        out.push_back(&expr);
    }
}

std::vector<const ExprNode*> splitConjunction(const ExprNode& expr) {
    std::vector<const ExprNode*> parts;
    splitConj(expr, parts);
    return parts;
}

Expr joinConjunction(std::vector<Expr> preds) {
    assert(!preds.empty());
    if (preds.size() == 1) return std::move(preds[0]);
    // Build a left-deep AND tree
    Expr root = std::move(preds[0]);
    for (size_t i = 1; i < preds.size(); ++i) {
        BinaryExpr andExpr;
        andExpr.op    = BinaryOp::AND;
        andExpr.left  = std::move(root);
        andExpr.right = std::move(preds[i]);
        root = std::make_unique<ExprNode>(std::move(andExpr));
    }
    return root;
}

bool isSatisfiedBy(const ExprNode& expr, const Schema& schema) {
    ColumnSet cols;
    collectColumns(expr, cols);
    for (const auto& col : cols) {
        bool found = schema.indexOf(col) >= 0;
        if (!found) return false;
    }
    return true;
}

bool rejectsNulls(const ExprNode& expr, std::string_view tableAlias) {
    // A predicate rejects NULLs from `tableAlias` if it references at least
    // one column from that table inside a null-rejecting context.
    return std::visit([&](const auto& e) -> bool {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ColumnRef>) {
            return e.table && *e.table == tableAlias;
        }
        if constexpr (std::is_same_v<T, IsNullExpr>)
            return e.negated && rejectsNulls(*e.expr, tableAlias);
        if constexpr (std::is_same_v<T, BinaryExpr>) {
            // Comparison operators reject NULLs on either side
            if (e.op >= BinaryOp::EQ && e.op <= BinaryOp::GTE)
                return rejectsNulls(*e.left, tableAlias) ||
                       rejectsNulls(*e.right, tableAlias);
            if (e.op == BinaryOp::AND)
                return rejectsNulls(*e.left, tableAlias) ||
                       rejectsNulls(*e.right, tableAlias);
        }
        return false;
    }, expr);
}

Expr cloneExpr(const ExprNode& expr) {
    return std::visit([](const auto& e) -> Expr {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, Literal>)
            return std::make_unique<ExprNode>(e);

        if constexpr (std::is_same_v<T, ColumnRef>)
            return std::make_unique<ExprNode>(e);

        if constexpr (std::is_same_v<T, UnaryExpr>) {
            UnaryExpr copy;
            copy.op       = e.op;
            copy.location = e.location;
            copy.operand  = cloneExpr(*e.operand);
            return std::make_unique<ExprNode>(std::move(copy));
        }

        if constexpr (std::is_same_v<T, BinaryExpr>) {
            BinaryExpr copy;
            copy.op       = e.op;
            copy.location = e.location;
            copy.left     = cloneExpr(*e.left);
            copy.right    = cloneExpr(*e.right);
            return std::make_unique<ExprNode>(std::move(copy));
        }

        if constexpr (std::is_same_v<T, IsNullExpr>) {
            IsNullExpr copy;
            copy.negated  = e.negated;
            copy.location = e.location;
            copy.expr     = cloneExpr(*e.expr);
            return std::make_unique<ExprNode>(std::move(copy));
        }

        if constexpr (std::is_same_v<T, CastExpr>) {
            CastExpr copy;
            copy.targetType = e.targetType;
            copy.location   = e.location;
            copy.expr       = cloneExpr(*e.expr);
            return std::make_unique<ExprNode>(std::move(copy));
        }

        // Fallback: return a ColumnRef placeholder (full impl clones all nodes)
        ColumnRef placeholder;
        placeholder.column = "__clone_placeholder__";
        return std::make_unique<ExprNode>(placeholder);
    }, expr);
}

} // namespace ExprUtils

// ============================================================================
// ConstantFolding
// ============================================================================

ast::Expr ConstantFolding::tryFold(const ExprNode& expr) {
    if (!ExprUtils::isConstant(expr)) return nullptr;
    // Don't fold literals — they're already as simple as possible
    if (std::holds_alternative<Literal>(expr)) return nullptr;

    try {
        Value v = ExprUtils::evalConstant(expr);
        return std::make_unique<ExprNode>(Literal{v, {}});
    } catch (...) {
        return nullptr;  // Folding failed (e.g. div-by-zero) — leave as-is
    }
}

bool ConstantFolding::foldInPlace(Expr& expr) {
    if (!expr) return false;
    bool mutated = false;

    // Recurse into children first (bottom-up)
    std::visit([&](auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, BinaryExpr>) {
            mutated |= foldInPlace(e.left);
            mutated |= foldInPlace(e.right);
        }
        if constexpr (std::is_same_v<T, UnaryExpr>)
            mutated |= foldInPlace(e.operand);
        if constexpr (std::is_same_v<T, CastExpr>)
            mutated |= foldInPlace(e.expr);
    }, *expr);

    // Try to fold this node
    if (auto folded = tryFold(*expr)) {
        expr    = std::move(folded);
        mutated = true;
    }
    return mutated;
}

bool ConstantFolding::apply(LogicalNode& node, RuleContext& ctx) {
    bool mutated = false;

    std::visit([&](auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, LogicalFilter>) {
            if (foldInPlace(n.predicate)) {
                mutated = true;
                ctx.changed();
            }
        }
        if constexpr (std::is_same_v<T, LogicalProject>) {
            for (auto& item : n.items) {
                if (foldInPlace(item.expr)) {
                    mutated = true;
                    ctx.changed();
                }
            }
        }
    }, node);

    return mutated;
}

// ============================================================================
// PredicateSimplify
// ============================================================================

Expr PredicateSimplify::simplifyExpr(Expr expr, bool& mutated) {
    if (!expr) return expr;

    // Bottom-up: simplify children first
    std::visit([&](auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, BinaryExpr>) {
            e.left  = simplifyExpr(std::move(e.left),  mutated);
            e.right = simplifyExpr(std::move(e.right), mutated);

            // a AND TRUE  → a
            if (e.op == BinaryOp::AND && ExprUtils::isAlwaysTrue(*e.right)) {
                expr = std::move(e.left); mutated = true; return;
            }
            if (e.op == BinaryOp::AND && ExprUtils::isAlwaysTrue(*e.left)) {
                expr = std::move(e.right); mutated = true; return;
            }
            // a AND FALSE → FALSE
            if (e.op == BinaryOp::AND && ExprUtils::isAlwaysFalse(*e.left)) {
                expr = std::make_unique<ExprNode>(Literal::boolean(false));
                mutated = true; return;
            }
            if (e.op == BinaryOp::AND && ExprUtils::isAlwaysFalse(*e.right)) {
                expr = std::make_unique<ExprNode>(Literal::boolean(false));
                mutated = true; return;
            }
            // a OR TRUE   → TRUE
            if (e.op == BinaryOp::OR && (ExprUtils::isAlwaysTrue(*e.left) ||
                                          ExprUtils::isAlwaysTrue(*e.right))) {
                expr = std::make_unique<ExprNode>(Literal::boolean(true));
                mutated = true; return;
            }
            // a OR FALSE  → a
            if (e.op == BinaryOp::OR && ExprUtils::isAlwaysFalse(*e.right)) {
                expr = std::move(e.left); mutated = true; return;
            }
            if (e.op == BinaryOp::OR && ExprUtils::isAlwaysFalse(*e.left)) {
                expr = std::move(e.right); mutated = true; return;
            }
        }
        if constexpr (std::is_same_v<T, UnaryExpr>) {
            e.operand = simplifyExpr(std::move(e.operand), mutated);
            // NOT (NOT x) → x
            if (e.op == UnaryOp::NOT) {
                auto* inner = std::get_if<UnaryExpr>(e.operand.get());
                if (inner && inner->op == UnaryOp::NOT) {
                    expr = std::move(inner->operand); mutated = true; return;
                }
                // NOT TRUE → FALSE
                if (ExprUtils::isAlwaysTrue(*e.operand)) {
                    expr = std::make_unique<ExprNode>(Literal::boolean(false));
                    mutated = true; return;
                }
                // NOT FALSE → TRUE
                if (ExprUtils::isAlwaysFalse(*e.operand)) {
                    expr = std::make_unique<ExprNode>(Literal::boolean(true));
                    mutated = true; return;
                }
            }
        }
    }, *expr);

    return expr;
}

bool PredicateSimplify::apply(LogicalNode& node, RuleContext& ctx) {
    auto* f = std::get_if<LogicalFilter>(&node);
    if (!f) return false;

    bool mutated = false;
    f->predicate = simplifyExpr(std::move(f->predicate), mutated);

    if (!mutated) return false;
    ctx.changed();

    // After simplification, check if the filter is trivially true/false.
    // The calling engine handles node replacement (we return true to re-run).
    return true;
}

// ============================================================================
// PredicatePushdown
// ============================================================================

bool PredicatePushdown::fuseIntoScan(LogicalScan& scan, Expr pred) {
    if (!scan.pushedPredicate) {
        scan.pushedPredicate = std::move(pred);
    } else {
        // AND the new predicate with the existing one
        BinaryExpr andExpr;
        andExpr.op    = BinaryOp::AND;
        andExpr.left  = std::move(scan.pushedPredicate);   // owns the existing predicate
        andExpr.right = std::move(pred);
        scan.pushedPredicate = std::make_unique<ExprNode>(std::move(andExpr));
    }
    return true;
}

bool PredicatePushdown::pushThroughProject(LogicalProject& node,
                                             Expr pred,
                                             RuleContext& ctx) {
    if (!node.child) return false;
    // A predicate can be pushed through a Project only if every column it
    // references exists in the child's output schema (i.e., the predicate
    // does not reference a computed column introduced by this Project).
    const Schema& childSch = outputSchema(*node.child);
    if (!ExprUtils::isSatisfiedBy(*pred, childSch)) return false;

    // Wrap child in a new Filter with the predicate
    Schema filterSch = childSch;
    LogicalFilter newFilter;
    newFilter.child       = std::move(node.child);
    newFilter.predicate   = std::move(pred);
    newFilter.outputSchema = filterSch;
    newFilter.nodeId      = ctx.catalog.allocNodeId();

    node.child = std::make_unique<LogicalNode>(std::move(newFilter));
    ctx.changed();
    return true;
}

bool PredicatePushdown::pushThroughJoin(LogicalJoin& join,
                                          Expr pred,
                                          RuleContext& ctx) {
    if (!join.left || !join.right) return false;

    const Schema& leftSch  = outputSchema(*join.left);
    const Schema& rightSch = outputSchema(*join.right);

    bool toLeft  = ExprUtils::isSatisfiedBy(*pred, leftSch);
    bool toRight = ExprUtils::isSatisfiedBy(*pred, rightSch);

    // For OUTER joins, only push to the non-null-extended side.
    // For simplicity in this milestone, only push through INNER / CROSS.
    if (join.type != ast::JoinType::INNER && join.type != ast::JoinType::CROSS)
        return false;

    if (toLeft && !toRight) {
        // Push entirely to the left child
        Schema leftFilterSch = leftSch;
        LogicalFilter lf;
        lf.child        = std::move(join.left);
        lf.predicate    = std::move(pred);
        lf.outputSchema = leftFilterSch;
        lf.nodeId       = ctx.catalog.allocNodeId();
        join.left = std::make_unique<LogicalNode>(std::move(lf));
        ctx.changed();
        return true;
    }
    if (toRight && !toLeft) {
        // Push entirely to the right child
        Schema rightFilterSch = rightSch;
        LogicalFilter rf;
        rf.child        = std::move(join.right);
        rf.predicate    = std::move(pred);
        rf.outputSchema = rightFilterSch;
        rf.nodeId       = ctx.catalog.allocNodeId();
        join.right = std::make_unique<LogicalNode>(std::move(rf));
        ctx.changed();
        return true;
    }
    if (!toLeft && !toRight) {
        // Predicate references columns from both sides → merge into join condition
        if (!join.condition) {
            join.condition = std::move(pred);
        } else {
            BinaryExpr andExpr;
            andExpr.op    = BinaryOp::AND;
            andExpr.left  = std::move(*join.condition);
            andExpr.right = std::move(pred);
            join.condition = std::make_unique<ExprNode>(std::move(andExpr));
        }
        ctx.changed();
        return true;
    }

    return false;  // Could go to either side — leave above join for now
}

bool PredicatePushdown::pushThroughAggregate(LogicalAggregate& agg,
                                               Expr pred,
                                               RuleContext& ctx) {
    if (!agg.child) return false;

    // A predicate can be pushed below an Aggregate only if it references
    // nothing but group-key columns (not aggregate outputs).
    // Aggregate output column names are the agg.aggCalls[i].outputName.
    ColumnSet predCols;
    ExprUtils::collectColumns(*pred, predCols);

    // Check none of the referenced columns are aggregate outputs
    for (const auto& agg_call : agg.aggCalls) {
        if (predCols.count(agg_call.outputName)) return false;  // post-agg col
    }

    // Safe to push into child
    const Schema& childSch = outputSchema(*agg.child);
    if (!ExprUtils::isSatisfiedBy(*pred, childSch)) return false;

    Schema filterSch = childSch;
    LogicalFilter newFilter;
    newFilter.child        = std::move(agg.child);
    newFilter.predicate    = std::move(pred);
    newFilter.outputSchema = filterSch;
    newFilter.nodeId       = ctx.catalog.allocNodeId();
    agg.child = std::make_unique<LogicalNode>(std::move(newFilter));
    ctx.changed();
    return true;
}

bool PredicatePushdown::pushThroughUnion(LogicalUnion& un,
                                           Expr pred,
                                           RuleContext& ctx) {
    if (!un.left || !un.right) return false;
    if (!un.all) return false;  // Only push through UNION ALL (not UNION DISTINCT)

    // Clone the predicate to push into both branches
    const Schema& leftSch  = outputSchema(*un.left);
    const Schema& rightSch = outputSchema(*un.right);

    Expr predLeft  = ExprUtils::cloneExpr(*pred);
    Expr predRight = std::move(pred);

    Schema leftFilterSch = leftSch;
    LogicalFilter lf;
    lf.child        = std::move(un.left);
    lf.predicate    = std::move(predLeft);
    lf.outputSchema = leftFilterSch;
    lf.nodeId       = ctx.catalog.allocNodeId();
    un.left = std::make_unique<LogicalNode>(std::move(lf));

    Schema rightFilterSch = rightSch;
    LogicalFilter rf;
    rf.child        = std::move(un.right);
    rf.predicate    = std::move(predRight);
    rf.outputSchema = rightFilterSch;
    rf.nodeId       = ctx.catalog.allocNodeId();
    un.right = std::make_unique<LogicalNode>(std::move(rf));

    ctx.changed();
    return true;
}

bool PredicatePushdown::apply(LogicalNode& node, RuleContext& ctx) {
    auto* f = std::get_if<LogicalFilter>(&node);
    if (!f || !f->child) return false;

    // Split the predicate into a conjunction of individual predicates.
    auto parts = ExprUtils::splitConjunction(*f->predicate);

    // Try to push each predicate part into the child.
    std::vector<Expr>          remaining;  // Predicates that could not be pushed
    bool                       anyPushed = false;

    for (const auto* part : parts) {
        Expr cloned = ExprUtils::cloneExpr(*part);
        bool pushed = std::visit([&](auto& child) -> bool {
            using T = std::decay_t<decltype(child)>;
            if constexpr (std::is_same_v<T, LogicalScan>)
                return fuseIntoScan(child, std::move(cloned));
            if constexpr (std::is_same_v<T, LogicalProject>)
                return pushThroughProject(child, std::move(cloned), ctx);
            if constexpr (std::is_same_v<T, LogicalJoin>)
                return pushThroughJoin(child, std::move(cloned), ctx);
            if constexpr (std::is_same_v<T, LogicalAggregate>)
                return pushThroughAggregate(child, std::move(cloned), ctx);
            if constexpr (std::is_same_v<T, LogicalUnion>)
                return pushThroughUnion(child, std::move(cloned), ctx);
            return false;
        }, *f->child);

        if (pushed) {
            anyPushed = true;
        } else {
            remaining.push_back(ExprUtils::cloneExpr(*part));
        }
    }

    if (!anyPushed) return false;

    ctx.changed();

    if (remaining.empty()) {
        // All predicates were pushed: replace this Filter with its child.
        // IMPORTANT: extract child BEFORE assigning to node — f points into node,
        // so assigning to node would destroy f mid-operation (UAF).
        LogicalPlan extracted = std::move(f->child);
        node = std::move(*extracted);
    } else {
        // Rebuild Filter with only the remaining predicates.
        f->predicate = ExprUtils::joinConjunction(std::move(remaining));
    }

    return true;
}

// ============================================================================
// OuterJoinSimplify
// ============================================================================

bool OuterJoinSimplify::apply(LogicalNode& node, RuleContext& ctx) {
    auto* f = std::get_if<LogicalFilter>(&node);
    if (!f || !f->child) return false;

    auto* join = std::get_if<LogicalJoin>(f->child.get());
    if (!join) return false;
    if (join->type == ast::JoinType::INNER || join->type == ast::JoinType::CROSS)
        return false;  // Already inner

    // Collect table aliases from the null-extended side
    auto getNullSideAlias = [&]() -> std::string {
        switch (join->type) {
            case ast::JoinType::LEFT:  return "right";
            case ast::JoinType::RIGHT: return "left";
            case ast::JoinType::FULL:  return "";  // Both sides are null-extended
            default: return "";
        }
    };

    const std::string nullSide = getNullSideAlias();
    if (nullSide.empty()) return false;

    // Get the alias of the null-extended child
    const LogicalNode* nullChild = (join->type == ast::JoinType::LEFT)
        ? join->right.get() : join->left.get();

    std::string tableAlias;
    const auto* scan = std::get_if<LogicalScan>(nullChild);
    if (scan) tableAlias = scan->alias.empty() ? scan->tableName : scan->alias;

    if (tableAlias.empty()) return false;

    // Check if the filter predicate rejects NULLs from the null-extended side
    if (!ExprUtils::rejectsNulls(*f->predicate, tableAlias)) return false;

    // Safe to convert to INNER JOIN
    join->type = ast::JoinType::INNER;
    ctx.changed();
    return true;
}

// ============================================================================
// ColumnPruning
// ============================================================================

ColumnSet ColumnPruning::propagateScan(LogicalScan& scan,
                                         const ColumnSet& required,
                                         RuleContext& ctx) {
    // Map required column names to their indices in the scan's output schema
    std::vector<size_t> needed;
    ColumnSet           produced;

    for (size_t i = 0; i < scan.outputSchema.size(); ++i) {
        const auto& col = scan.outputSchema[i];
        // Check both bare name and qualified name
        bool req = required.count(col.name) > 0 ||
                   required.count(scan.alias + "." + col.name) > 0 ||
                   required.count(scan.tableName + "." + col.name) > 0;
        if (req) {
            needed.push_back(i);
            produced.insert(col.name);
        }
    }

    if (needed.size() == scan.outputSchema.size()) {
        // No pruning possible — reading all columns already
        scan.columnPruning.clear();
        return produced;
    }

    if (!scan.columnPruning.empty() && scan.columnPruning == needed)
        return produced;  // No change

    scan.columnPruning = needed;
    ctx.changed();
    return produced;
}

ColumnSet ColumnPruning::propagateFilter(LogicalFilter& f,
                                           const ColumnSet& required,
                                           RuleContext& ctx) {
    if (!f.child) return required;

    // Filter needs: required ∪ columns referenced by predicate
    ColumnSet childRequired = required;
    ExprUtils::collectColumns(*f.predicate, childRequired);
    return propagate(*f.child, childRequired, ctx);
}

ColumnSet ColumnPruning::propagateProject(LogicalProject& p,
                                            const ColumnSet& required,
                                            RuleContext& ctx) {
    if (!p.child) return required;

    // Only compute project items that are actually required
    ColumnSet childRequired;
    for (const auto& item : p.items) {
        if (required.count(item.outputName)) {
            ExprUtils::collectColumns(*item.expr, childRequired);
        }
    }

    return propagate(*p.child, childRequired, ctx);
}

ColumnSet ColumnPruning::propagateAggregate(LogicalAggregate& agg,
                                              const ColumnSet& required,
                                              RuleContext& ctx) {
    if (!agg.child) return required;

    // Aggregate needs: all group key columns + all agg input columns
    ColumnSet childRequired;
    for (const auto& key : agg.groupKeys)
        ExprUtils::collectColumns(*key, childRequired);
    for (const auto& call : agg.aggCalls)
        for (const auto& arg : call.args)
            ExprUtils::collectColumns(*arg, childRequired);

    return propagate(*agg.child, childRequired, ctx);
}

ColumnSet ColumnPruning::propagateJoin(LogicalJoin& join,
                                         const ColumnSet& required,
                                         RuleContext& ctx) {
    if (!join.left || !join.right) return required;

    const Schema& leftSch  = outputSchema(*join.left);
    const Schema& rightSch = outputSchema(*join.right);

    ColumnSet joinCond;
    if (join.condition)
        ExprUtils::collectColumns(**join.condition, joinCond);

    // Split required + joinCond columns between left and right
    ColumnSet leftReq, rightReq;
    auto classify = [&](const std::string& col) {
        if (leftSch.indexOf(col)  >= 0) leftReq.insert(col);
        if (rightSch.indexOf(col) >= 0) rightReq.insert(col);
    };
    for (const auto& c : required)  classify(c);
    for (const auto& c : joinCond)  classify(c);

    ColumnSet lProduced = propagate(*join.left,  leftReq,  ctx);
    ColumnSet rProduced = propagate(*join.right, rightReq, ctx);

    // Union of produced sets
    ColumnSet produced = lProduced;
    produced.insert(rProduced.begin(), rProduced.end());
    return produced;
}

ColumnSet ColumnPruning::propagateUnion(LogicalUnion& un,
                                          const ColumnSet& required,
                                          RuleContext& ctx) {
    if (un.left)  propagate(*un.left,  required, ctx);
    if (un.right) propagate(*un.right, required, ctx);
    return required;
}

ColumnSet ColumnPruning::propagateSort(LogicalSort& sort,
                                         const ColumnSet& required,
                                         RuleContext& ctx) {
    if (!sort.child) return required;

    // Sort needs: required ∪ sort key columns
    ColumnSet childRequired = required;
    for (const auto& key : sort.keys)
        ExprUtils::collectColumns(*key.expr, childRequired);

    return propagate(*sort.child, childRequired, ctx);
}

ColumnSet ColumnPruning::propagateLimit(LogicalLimit& lim,
                                          const ColumnSet& required,
                                          RuleContext& ctx) {
    if (!lim.child) return required;
    return propagate(*lim.child, required, ctx);
}

ColumnSet ColumnPruning::propagate(LogicalNode& node,
                                     const ColumnSet& required,
                                     RuleContext& ctx) {
    return std::visit([&](auto& n) -> ColumnSet {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, LogicalScan>)
            return propagateScan(n, required, ctx);
        if constexpr (std::is_same_v<T, LogicalFilter>)
            return propagateFilter(n, required, ctx);
        if constexpr (std::is_same_v<T, LogicalProject>)
            return propagateProject(n, required, ctx);
        if constexpr (std::is_same_v<T, LogicalAggregate>)
            return propagateAggregate(n, required, ctx);
        if constexpr (std::is_same_v<T, LogicalJoin>)
            return propagateJoin(n, required, ctx);
        if constexpr (std::is_same_v<T, LogicalUnion>)
            return propagateUnion(n, required, ctx);
        if constexpr (std::is_same_v<T, LogicalSort>)
            return propagateSort(n, required, ctx);
        if constexpr (std::is_same_v<T, LogicalLimit>)
            return propagateLimit(n, required, ctx);
        return required;
    }, node);
}

bool ColumnPruning::apply(LogicalNode& node, RuleContext& ctx) {
    // ColumnPruning is top-down: we start from the root with all output
    // columns required (i.e., we don't know what the caller needs, so we
    // assume all columns in the root's output schema are required).
    if (!firstRun_) return false;  // Run exactly once per optimize() call
    firstRun_ = false;

    const Schema& rootSch = outputSchema(node);
    ColumnSet allRequired;
    for (size_t i = 0; i < rootSch.size(); ++i)
        allRequired.insert(rootSch[i].name);

    int before = ctx.mutationCount;
    propagate(node, allRequired, ctx);
    return ctx.mutationCount > before;
}

// ============================================================================
// LimitPushdown
// ============================================================================

bool LimitPushdown::apply(LogicalNode& node, RuleContext& ctx) {
    auto* lim = std::get_if<LogicalLimit>(&node);
    if (!lim || !lim->limit || !lim->child) return false;

    auto* sort = std::get_if<LogicalSort>(lim->child.get());
    if (!sort) return false;

    // Annotate the Sort node with the limit value so the executor can use
    // a top-K heap instead of a full sort.
    // We encode this as a negative nodeId convention — in production you'd
    // add a topK field to LogicalSort. Here we reuse the nodeId upper bits.
    // Real implementation: sort.topK = lim->limit;
    // For now, signal to the executor via the stats machinery.
    // This is a placeholder that the physical planner will honour.
    (void)sort;  // topK annotation goes here when LogicalSort has the field
    return false; // No structural change yet — flag for physical planner only
}

// ============================================================================
// ProjectMerge
// ============================================================================

bool ProjectMerge::apply(LogicalNode& node, RuleContext& ctx) {
    auto* outer = std::get_if<LogicalProject>(&node);
    if (!outer || !outer->child) return false;

    auto* inner = std::get_if<LogicalProject>(outer->child.get());
    if (!inner) return false;

    // Build a substitution map: inner output name → inner expr
    std::unordered_map<std::string, const ExprNode*> innerMap;
    for (const auto& item : inner->items)
        innerMap[item.outputName] = item.expr.get();

    // For each outer item, if it's just a ColumnRef to an inner output,
    // replace it with the inner expression.
    bool mutated = false;
    for (auto& outerItem : outer->items) {
        const auto* ref = std::get_if<ColumnRef>(outerItem.expr.get());
        if (!ref) continue;
        auto it = innerMap.find(ref->column);
        if (it == innerMap.end()) continue;

        // Replace outer's ColumnRef with a clone of the inner expression
        outerItem.expr = ExprUtils::cloneExpr(*it->second);
        mutated = true;
    }

    if (!mutated) return false;

    // The inner Project is no longer needed — promote its child
    outer->child = std::move(inner->child);
    ctx.changed();
    return true;
}

// ============================================================================
// OptimizerStats
// ============================================================================

std::string OptimizerStats::toString() const {
    std::ostringstream os;
    os << "Optimizer: " << totalPasses << " passes, "
       << totalMutations << " total mutations\n";
    for (const auto& r : rules) {
        if (r.applications == 0) continue;
        os << "  " << r.ruleName << ": "
           << r.applications << " applications, "
           << r.mutations    << " mutations\n";
    }
    return os.str();
}

// ============================================================================
// Optimizer driver
// ============================================================================

Optimizer::Optimizer(const Catalog& catalog, bool debug)
    : catalog_(catalog), debug_(debug) {}

void Optimizer::addRule(std::unique_ptr<Rule> rule) {
    stats_.rules.push_back({std::string(rule->name()), 0, 0});
    rules_.push_back(std::move(rule));
}

void Optimizer::addDefaultRules() {
    // Pass 1 — Normalization
    addRule(std::make_unique<ConstantFolding>());
    addRule(std::make_unique<PredicateSimplify>());
    // Pass 2 — Predicate pushdown
    addRule(std::make_unique<OuterJoinSimplify>());
    addRule(std::make_unique<PredicatePushdown>());
    // Pass 3 — Column pruning (top-down, runs once)
    addRule(std::make_unique<ColumnPruning>());
    // Pass 4 — Algebraic simplification
    addRule(std::make_unique<ProjectMerge>());
    addRule(std::make_unique<LimitPushdown>());
}

void Optimizer::clearRules() {
    rules_.clear();
    stats_.rules.clear();
}

bool Optimizer::applyToChildren(Rule& rule, LogicalNode& node, RuleContext& ctx) {
    bool mutated = false;
    std::visit([&](auto& n) {
        using T = std::decay_t<decltype(n)>;
        // Apply to each child field that is a LogicalPlan
        if constexpr (std::is_same_v<T, LogicalFilter>) {
            if (n.child) mutated |= applyRule(rule, *n.child, ctx);
        }
        if constexpr (std::is_same_v<T, LogicalProject>) {
            if (n.child) mutated |= applyRule(rule, *n.child, ctx);
        }
        if constexpr (std::is_same_v<T, LogicalAggregate>) {
            if (n.child) mutated |= applyRule(rule, *n.child, ctx);
        }
        if constexpr (std::is_same_v<T, LogicalSort>) {
            if (n.child) mutated |= applyRule(rule, *n.child, ctx);
        }
        if constexpr (std::is_same_v<T, LogicalLimit>) {
            if (n.child) mutated |= applyRule(rule, *n.child, ctx);
        }
        if constexpr (std::is_same_v<T, LogicalDistinct>) {
            if (n.child) mutated |= applyRule(rule, *n.child, ctx);
        }
        if constexpr (std::is_same_v<T, LogicalJoin>) {
            if (n.left)  mutated |= applyRule(rule, *n.left,  ctx);
            if (n.right) mutated |= applyRule(rule, *n.right, ctx);
        }
        if constexpr (std::is_same_v<T, LogicalUnion>) {
            if (n.left)  mutated |= applyRule(rule, *n.left,  ctx);
            if (n.right) mutated |= applyRule(rule, *n.right, ctx);
        }
    }, node);
    return mutated;
}

bool Optimizer::applyBottomUp(Rule& rule, LogicalNode& node, RuleContext& ctx) {
    bool mutated = applyToChildren(rule, node, ctx);
    mutated     |= rule.apply(node, ctx);
    return mutated;
}

bool Optimizer::applyTopDown(Rule& rule, LogicalNode& node, RuleContext& ctx) {
    bool mutated = rule.apply(node, ctx);
    mutated     |= applyToChildren(rule, node, ctx);
    return mutated;
}

bool Optimizer::applyRule(Rule& rule, LogicalNode& node, RuleContext& ctx) {
    return rule.isTopDown()
        ? applyTopDown(rule, node, ctx)
        : applyBottomUp(rule, node, ctx);
}

LogicalPlan Optimizer::optimize(LogicalPlan plan) {
    if (!plan) return plan;

    stats_.totalPasses    = 0;
    stats_.totalMutations = 0;
    for (auto& r : stats_.rules) r.applications = r.mutations = 0;

    RuleContext ctx{catalog_, 0, 0, debug_};

    for (int pass = 0; pass < kMaxPasses; ++pass) {
        ctx.resetMutations();
        ctx.passNumber = pass;

        // Reset ColumnPruning's firstRun_ flag each pass so it runs once per pass
        for (auto& rule : rules_) {
            if (auto* cp = dynamic_cast<ColumnPruning*>(rule.get()))
                cp->firstRun_ = true;  // allow re-run on new pass
        }

        for (size_t ri = 0; ri < rules_.size(); ++ri) {
            Rule& rule  = *rules_[ri];
            int before  = ctx.mutationCount;
            applyRule(rule, *plan, ctx);
            int fired   = ctx.mutationCount - before;

            stats_.rules[ri].applications++;
            stats_.rules[ri].mutations += fired;

            if (debug_ && fired > 0) {
                std::cerr << "[opt pass " << pass << "] "
                          << rule.name() << " fired " << fired << " times\n";
            }
        }

        ++stats_.totalPasses;
        stats_.totalMutations += ctx.mutationCount;

        if (!ctx.anyMutation()) break;  // Fixed point reached
    }

    return plan;
}

} // namespace opt
} // namespace lq
