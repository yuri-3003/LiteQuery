#pragma once

// ============================================================================
// LiteQuery — physical_plan.h
// Vectorized pull-model physical operators.
//
// Execution model
// ───────────────
// Operators form a tree. Each implements Operator::next(), returning one Batch
// of up to kDefaultBatchSize rows (columnar). The root is pulled in a loop
// until it returns an empty batch (end of stream). Data flows child → parent;
// control flows parent → child. This is the classic "Volcano" pull model with
// batches instead of single tuples, so per-call overhead is amortized.
//
//   result ← root.next()   // repeatedly, until empty
//     root pulls from its child, which pulls from its child, … down to a scan.
//
// Blocking vs streaming
// ─────────────────────
//   Streaming (one batch in → one batch out): SeqScan, Filter, Project, Limit.
//   Blocking  (must consume the whole child first): HashAggregate, Sort,
//   Distinct, HashJoin (builds a hash table on its right/build side).
//
// Operators reference expressions from the AST (owned by the SelectStmt that
// outlives execution) — they never own AST nodes.
// ============================================================================

#include "ast.h"
#include "eval.h"
#include "table.h"
#include "types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lq {
namespace exec {

// ============================================================================
// Operator — abstract pull-model operator
// ============================================================================

class Operator {
public:
    virtual ~Operator() = default;

    // The schema of the batches this operator produces.
    virtual const Schema& schema() const = 0;

    // Produce the next batch. An empty batch (numRows == 0) means end-of-stream.
    virtual Batch next() = 0;

    // Rewind to the beginning (optional; used by nested-loop-style re-scans).
    virtual void reset() {}

    // Render this operator (and its children) as an indented tree — the basis
    // of EXPLAIN. Each operator prints one line for itself, then its children
    // at indent+1.
    virtual std::string explain(int indent) const = 0;

protected:
    static std::string indentStr(int n) { return std::string(n * 2, ' '); }
};

using OperatorPtr = std::unique_ptr<Operator>;

// ============================================================================
// SeqScan — read all rows of a base table, column by column
// ============================================================================

class SeqScan : public Operator {
public:
    // `outputSchema` names may be alias-qualified ("t.col"); values come from
    // the underlying table columns in order.
    SeqScan(TablePtr table, Schema outputSchema);

    const Schema& schema() const override { return schema_; }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { cursor_ = 0; }

private:
    TablePtr table_;
    Schema   schema_;
    size_t   cursor_ = 0;
};

// ============================================================================
// Filter — keep rows for which `predicate` is true
// ============================================================================

class Filter : public Operator {
public:
    Filter(OperatorPtr child, const ast::ExprNode* predicate);

    const Schema& schema() const override { return child_->schema(); }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { child_->reset(); }

private:
    OperatorPtr           child_;
    const ast::ExprNode*  predicate_;
};

// ============================================================================
// Project — compute output expressions
// ============================================================================

class Project : public Operator {
public:
    struct Item {
        const ast::ExprNode* expr;   // borrowed from the AST
        std::string          name;
        DataType             type;
    };

    Project(OperatorPtr child, std::vector<Item> items);

    const Schema& schema() const override { return schema_; }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { child_->reset(); }

private:
    OperatorPtr       child_;
    std::vector<Item> items_;
    Schema            schema_;
};

// ============================================================================
// Limit — pass at most `limit` rows after skipping `offset`
// ============================================================================

class Limit : public Operator {
public:
    Limit(OperatorPtr child, int64_t limit, int64_t offset);

    const Schema& schema() const override { return child_->schema(); }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { child_->reset(); emitted_ = 0; skipped_ = 0; }

private:
    OperatorPtr child_;
    int64_t     limit_;
    int64_t     offset_;
    int64_t     emitted_ = 0;
    int64_t     skipped_ = 0;
};

// ============================================================================
// Sort — full ORDER BY sort (blocking). key exprs + directions.
// ============================================================================

class Sort : public Operator {
public:
    struct Key {
        const ast::ExprNode* expr;
        ast::SortOrder       order;
        ast::NullsOrder      nulls;
    };

    Sort(OperatorPtr child, std::vector<Key> keys);

    const Schema& schema() const override { return child_->schema(); }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { built_ = false; cursor_ = 0; rows_.clear(); }

private:
    void build();

    OperatorPtr      child_;
    std::vector<Key> keys_;
    std::vector<Row> rows_;
    bool             built_ = false;
    size_t           cursor_ = 0;
};

// ============================================================================
// Distinct — deduplicate rows (blocking; keeps first occurrence)
// ============================================================================

class Distinct : public Operator {
public:
    explicit Distinct(OperatorPtr child);

    const Schema& schema() const override { return child_->schema(); }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { built_ = false; cursor_ = 0; rows_.clear(); }

private:
    void build();

    OperatorPtr      child_;
    std::vector<Row> rows_;
    bool             built_ = false;
    size_t           cursor_ = 0;
};

// ============================================================================
// HashAggregate — GROUP BY + aggregate functions (blocking)
//
// Output schema = [group key columns...] ++ [aggregate result columns...].
// Supports SUM, COUNT, COUNT(*), AVG, MIN, MAX (and COUNT DISTINCT).
// ============================================================================

class HashAggregate : public Operator {
public:
    struct Agg {
        std::string          func;     // "SUM" | "COUNT" | "AVG" | "MIN" | "MAX"
        const ast::ExprNode* arg;      // argument expr (null for COUNT(*))
        bool                 star;
        bool                 distinct;
        std::string          outName;
        DataType             outType;
    };

    HashAggregate(OperatorPtr child,
                  std::vector<const ast::ExprNode*> groupKeys,
                  std::vector<std::string>          groupKeyNames,
                  std::vector<Agg>                  aggs);

    const Schema& schema() const override { return schema_; }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { built_ = false; cursor_ = 0; groups_.clear(); order_.clear(); }

private:
    void build();

    OperatorPtr                        child_;
    std::vector<const ast::ExprNode*>  groupKeys_;
    std::vector<Agg>                   aggs_;
    Schema                             schema_;

    // Accumulator state for one group.
    struct AccState {
        std::vector<double>  numAcc;    // running sum / min / max as double
        std::vector<int64_t> counts;    // per-agg count of non-null inputs
        std::vector<Value>   minmax;    // for MIN/MAX (typed)
        std::vector<std::unordered_map<std::string,bool>> seen; // COUNT DISTINCT
        Row                  keyValues; // the group key values
    };

    std::unordered_map<std::string, AccState> groups_;
    std::vector<std::string>                  order_;   // stable group emit order
    bool   built_ = false;
    size_t cursor_ = 0;
};

// ============================================================================
// HashJoin — INNER / LEFT / RIGHT / FULL / CROSS join (blocking on build side)
//
// Builds a hash table on the right (build) input keyed by the equi-join column,
// then probes with the left. CROSS and non-equi conditions fall back to a
// nested-loop evaluation of the ON predicate.
// ============================================================================

class HashJoin : public Operator {
public:
    HashJoin(OperatorPtr left, OperatorPtr right,
             ast::JoinType type,
             const ast::ExprNode* condition,   // null for CROSS
             Schema outputSchema);

    const Schema& schema() const override { return schema_; }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override;

private:
    void build();
    void materializeLeft();

    OperatorPtr          left_;
    OperatorPtr          right_;
    ast::JoinType        type_;
    const ast::ExprNode* condition_;
    Schema               schema_;
    Schema               leftSchema_;
    Schema               rightSchema_;

    std::vector<Row>     leftRows_;
    std::vector<Row>     rightRows_;
    std::vector<char>    rightMatched_;   // for RIGHT/FULL outer
    bool                 built_ = false;
    size_t               cursor_ = 0;

    // Result is materialized eagerly (MVP); streamed out in batches.
    std::vector<Row>     result_;
    void computeResult();
};

// ============================================================================
// Append — concatenate two inputs (UNION ALL). Streams the left child to
// exhaustion, then the right. The output schema is the left child's; the right
// child must produce the same number of columns (values are passed through
// positionally, per SQL UNION semantics). Wrap in Distinct for plain UNION.
// ============================================================================

class Append : public Operator {
public:
    Append(OperatorPtr left, OperatorPtr right);

    const Schema& schema() const override { return left_->schema(); }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { left_->reset(); right_->reset(); leftDone_ = false; }

private:
    OperatorPtr left_;
    OperatorPtr right_;
    bool        leftDone_ = false;
};

// ============================================================================
// Values — a constant single-row (or multi-row) source, and the "dual" table
// ============================================================================

class Values : public Operator {
public:
    Values(Schema schema, std::vector<Row> rows);

    const Schema& schema() const override { return schema_; }
    Batch next() override;
    std::string explain(int indent) const override;
    void  reset() override { cursor_ = 0; }

private:
    Schema           schema_;
    std::vector<Row> rows_;
    size_t           cursor_ = 0;
};

// ============================================================================
// Driver — pull the whole operator tree into one materialized result table.
// ============================================================================

// Collect every row the operator produces into a single Batch.
Batch drain(Operator& root);

}  // namespace exec
}  // namespace lq
