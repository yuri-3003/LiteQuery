#pragma once

// ============================================================================
// LiteQuery — optimizer.h
// Rule-based, fixed-point logical optimizer.
//
// The optimizer rewrites a LogicalPlan in place by repeatedly applying a set
// of Rules until no rule fires (a fixed point) or a pass cap is hit. Each Rule
// is a local tree rewrite; the driver handles traversal (top-down vs bottom-up)
// and convergence.
//
// Rules currently implemented:
//   ConstantFolding    — fold constant sub-expressions to literals
//   PredicateSimplify  — algebraic simplification of boolean predicates
//   OuterJoinSimplify  — demote outer joins to inner when a predicate rejects nulls
//   PredicatePushdown  — push filters toward the scans (through project/join/…)
//   ColumnPruning      — drop columns no ancestor needs (top-down, once/pass)
//   ProjectMerge       — collapse adjacent projections
//   LimitPushdown      — annotate sorts feeding a limit for top-K execution
//
// Adding a rule: derive from Rule, implement apply()/name()/isTopDown(), and
// register it via Optimizer::addRule().
// ============================================================================

#include "logical_plan.h"
#include "catalog.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lq {
namespace opt {

// ============================================================================
// ColumnSet — a set of column names required by some part of the plan.
// Names are stored in both qualified ("t.a") and unqualified ("a") forms.
// ============================================================================

using ColumnSet = std::unordered_set<std::string>;

// ============================================================================
// ExprUtils — pure helpers on AST expressions used by several rules
// ============================================================================

namespace ExprUtils {

// True if the expression contains no column references or side-effecting calls.
bool isConstant(const ast::ExprNode& expr) noexcept;

// Evaluate a constant expression to a Value (NULL on failure/undefined).
Value evalConstant(const ast::ExprNode& expr);

// Deep-copy an expression tree.
ast::Expr cloneExpr(const ast::ExprNode& expr);

// True if `expr` is a boolean literal true / a null-or-false literal.
bool isAlwaysTrue(const ast::ExprNode& expr) noexcept;
bool isAlwaysFalse(const ast::ExprNode& expr) noexcept;

// True if every column `expr` references is present in `schema`.
bool isSatisfiedBy(const ast::ExprNode& expr, const Schema& schema);

}  // namespace ExprUtils

// Gather every column name referenced by `expr` into `out`.
void collectColumns(const ast::ExprNode& expr, ColumnSet& out);

// Split a conjunction (a AND b AND c) into its leaf predicates (by pointer).
std::vector<const ast::ExprNode*> splitConjunction(const ast::ExprNode& expr);

// Rebuild a left-deep AND tree from a non-empty list of predicates.
ast::Expr joinConjunction(std::vector<ast::Expr> preds);

// True if `expr` evaluates to NULL/false whenever a column of `tableAlias`
// is NULL (used to demote outer joins to inner joins).
bool rejectsNulls(const ast::ExprNode& expr, std::string_view tableAlias);

// ============================================================================
// RuleContext — per-optimize() shared state threaded to every rule
// ============================================================================

struct RuleContext {
    const Catalog& catalog;
    int  mutationCount = 0;   // Rewrites applied in the current pass
    int  passNumber    = 0;   // 0-based pass index
    bool debug         = false;

    // A rule calls changed() whenever it rewrites the tree.
    void changed() noexcept { ++mutationCount; }
    void resetMutations() noexcept { mutationCount = 0; }
    bool anyMutation() const noexcept { return mutationCount > 0; }
};

// ============================================================================
// Rule — base class for a single logical rewrite
// ============================================================================

class Rule {
public:
    virtual ~Rule() = default;

    // Attempt to rewrite `node`. Return true if the tree was mutated.
    // The driver invokes this once per node during traversal.
    virtual bool apply(plan::LogicalNode& node, RuleContext& ctx) = 0;

    // Human-readable rule name (for stats / debug output).
    virtual std::string_view name() const noexcept = 0;

    // Traversal order: true = visit parent before children, false = children first.
    virtual bool isTopDown() const noexcept { return false; }
};

// ---- Concrete rules --------------------------------------------------------

class ConstantFolding : public Rule {
public:
    bool apply(plan::LogicalNode& node, RuleContext& ctx) override;
    std::string_view name() const noexcept override { return "ConstantFolding"; }

private:
    ast::Expr tryFold(const ast::ExprNode& expr);
    bool      foldInPlace(ast::Expr& expr);
};

class PredicateSimplify : public Rule {
public:
    bool apply(plan::LogicalNode& node, RuleContext& ctx) override;
    std::string_view name() const noexcept override { return "PredicateSimplify"; }

private:
    ast::Expr simplifyExpr(ast::Expr expr, bool& mutated);
};

class OuterJoinSimplify : public Rule {
public:
    bool apply(plan::LogicalNode& node, RuleContext& ctx) override;
    std::string_view name() const noexcept override { return "OuterJoinSimplify"; }
};

class PredicatePushdown : public Rule {
public:
    bool apply(plan::LogicalNode& node, RuleContext& ctx) override;
    std::string_view name() const noexcept override { return "PredicatePushdown"; }

private:
    bool fuseIntoScan(plan::LogicalScan& scan, ast::Expr pred);
    bool pushThroughProject(plan::LogicalProject& node, ast::Expr pred, RuleContext& ctx);
    bool pushThroughJoin(plan::LogicalJoin& join, ast::Expr pred, RuleContext& ctx);
    bool pushThroughAggregate(plan::LogicalAggregate& agg, ast::Expr pred, RuleContext& ctx);
    bool pushThroughUnion(plan::LogicalUnion& un, ast::Expr pred, RuleContext& ctx);
};

class ColumnPruning : public Rule {
public:
    bool apply(plan::LogicalNode& node, RuleContext& ctx) override;
    std::string_view name() const noexcept override { return "ColumnPruning"; }
    bool isTopDown() const noexcept override { return true; }

    // Reset by the driver at the start of each pass so the rule runs once/pass.
    bool firstRun_ = true;

private:
    ColumnSet propagate(plan::LogicalNode& node, const ColumnSet& required, RuleContext& ctx);
    ColumnSet propagateScan(plan::LogicalScan& scan, const ColumnSet& required, RuleContext& ctx);
    ColumnSet propagateFilter(plan::LogicalFilter& f, const ColumnSet& required, RuleContext& ctx);
    ColumnSet propagateProject(plan::LogicalProject& p, const ColumnSet& required, RuleContext& ctx);
    ColumnSet propagateAggregate(plan::LogicalAggregate& agg, const ColumnSet& required, RuleContext& ctx);
    ColumnSet propagateJoin(plan::LogicalJoin& join, const ColumnSet& required, RuleContext& ctx);
    ColumnSet propagateUnion(plan::LogicalUnion& un, const ColumnSet& required, RuleContext& ctx);
    ColumnSet propagateSort(plan::LogicalSort& sort, const ColumnSet& required, RuleContext& ctx);
    ColumnSet propagateLimit(plan::LogicalLimit& lim, const ColumnSet& required, RuleContext& ctx);
};

class ProjectMerge : public Rule {
public:
    bool apply(plan::LogicalNode& node, RuleContext& ctx) override;
    std::string_view name() const noexcept override { return "ProjectMerge"; }
};

class LimitPushdown : public Rule {
public:
    bool apply(plan::LogicalNode& node, RuleContext& ctx) override;
    std::string_view name() const noexcept override { return "LimitPushdown"; }
};

// ============================================================================
// OptimizerStats — per-rule and aggregate counters for one optimize() call
// ============================================================================

struct OptimizerStats {
    struct RuleStat {
        std::string ruleName;
        int         applications = 0;
        int         mutations    = 0;
    };

    int                   totalPasses    = 0;
    int                   totalMutations = 0;
    std::vector<RuleStat> rules;

    std::string toString() const;
};

// ============================================================================
// Optimizer — the fixed-point driver
// ============================================================================

class Optimizer {
public:
    explicit Optimizer(const Catalog& catalog, bool debug = false);

    // Register the standard rule pipeline (in canonical order).
    void addDefaultRules();
    void addRule(std::unique_ptr<Rule> rule);
    void clearRules();

    // Rewrite `plan` to a fixed point and return the (same, mutated) plan.
    plan::LogicalPlan optimize(plan::LogicalPlan plan);

    const OptimizerStats& stats() const noexcept { return stats_; }

private:
    // Traversal helpers.
    bool applyRule(Rule& rule, plan::LogicalNode& node, RuleContext& ctx);
    bool applyToChildren(Rule& rule, plan::LogicalNode& node, RuleContext& ctx);
    bool applyBottomUp(Rule& rule, plan::LogicalNode& node, RuleContext& ctx);
    bool applyTopDown(Rule& rule, plan::LogicalNode& node, RuleContext& ctx);

    static constexpr int kMaxPasses = 16;

    const Catalog&                     catalog_;
    bool                               debug_;
    std::vector<std::unique_ptr<Rule>> rules_;
    OptimizerStats                     stats_;
};

}  // namespace opt
}  // namespace lq
