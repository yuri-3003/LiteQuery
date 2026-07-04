// ============================================================================
// LiteQuery — physical_plan.cpp
// Pull-model operator implementations.
// ============================================================================

#include "physical_plan.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lq {
namespace exec {

using namespace ast;

// ============================================================================
// SeqScan
// ============================================================================

SeqScan::SeqScan(TablePtr table, Schema outputSchema)
    : table_(std::move(table)), schema_(std::move(outputSchema)) {}

Batch SeqScan::next() {
    Batch batch(schema_);
    const size_t total = table_->rowCount();
    if (cursor_ >= total) return batch;   // empty → EOS

    const size_t end = std::min(cursor_ + kDefaultBatchSize, total);
    const size_t n   = end - cursor_;
    const size_t ncol = table_->columnCount();

    for (size_t c = 0; c < ncol; ++c) {
        const Column& col = table_->columnAt(c);
        auto& out = batch.columns[c];
        out.reserve(n);
        for (size_t r = cursor_; r < end; ++r)
            out.push_back(col[r]);
    }
    batch.numRows = n;
    cursor_ = end;
    return batch;
}

// ============================================================================
// Filter
// ============================================================================

Filter::Filter(OperatorPtr child, const ast::ExprNode* predicate)
    : child_(std::move(child)), predicate_(predicate) {}

Batch Filter::next() {
    const Schema& sch = child_->schema();
    // Pull child batches until we accumulate at least one surviving row or EOS.
    for (;;) {
        Batch in = child_->next();
        if (in.empty()) return Batch(sch);   // EOS

        Batch out(sch);
        for (size_t r = 0; r < in.numRows; ++r) {
            Row row = in.getRow(r);
            if (evaluatePredicate(*predicate_, sch, row))
                out.appendRow(std::move(row));
        }
        if (!out.empty()) return out;
        // else: whole batch filtered out — loop for the next one
    }
}

// ============================================================================
// Project
// ============================================================================

Project::Project(OperatorPtr child, std::vector<Item> items)
    : child_(std::move(child)), items_(std::move(items)) {
    for (const auto& it : items_)
        schema_.addColumn(it.name, it.type);
}

Batch Project::next() {
    Batch in = child_->next();
    if (in.empty()) return Batch(schema_);

    const Schema& inSch = child_->schema();
    Batch out(schema_);
    for (auto& col : out.columns) col.reserve(in.numRows);

    for (size_t r = 0; r < in.numRows; ++r) {
        Row row = in.getRow(r);
        for (size_t c = 0; c < items_.size(); ++c)
            out.columns[c].push_back(evaluate(*items_[c].expr, inSch, row));
    }
    out.numRows = in.numRows;
    return out;
}

// ============================================================================
// Limit
// ============================================================================

Limit::Limit(OperatorPtr child, int64_t limit, int64_t offset)
    : child_(std::move(child)), limit_(limit), offset_(offset) {}

Batch Limit::next() {
    const Schema& sch = child_->schema();
    if (limit_ >= 0 && emitted_ >= limit_) return Batch(sch);

    for (;;) {
        Batch in = child_->next();
        if (in.empty()) return Batch(sch);

        Batch out(sch);
        for (size_t r = 0; r < in.numRows; ++r) {
            if (skipped_ < offset_) { ++skipped_; continue; }
            if (limit_ >= 0 && emitted_ >= limit_) break;
            out.appendRow(in.getRow(r));
            ++emitted_;
        }
        if (!out.empty()) return out;
        if (limit_ >= 0 && emitted_ >= limit_) return Batch(sch);
        // else keep pulling (we were still inside the offset window)
    }
}

// ============================================================================
// Sort
// ============================================================================

Sort::Sort(OperatorPtr child, std::vector<Key> keys)
    : child_(std::move(child)), keys_(std::move(keys)) {}

void Sort::build() {
    for (;;) {
        Batch in = child_->next();
        if (in.empty()) break;
        for (size_t r = 0; r < in.numRows; ++r)
            rows_.push_back(in.getRow(r));
    }

    const Schema& sch = child_->schema();
    std::stable_sort(rows_.begin(), rows_.end(),
        [&](const Row& a, const Row& b) {
            for (const auto& k : keys_) {
                Value va = evaluate(*k.expr, sch, a);
                Value vb = evaluate(*k.expr, sch, b);

                // NULL ordering
                bool an = va.isNull(), bn = vb.isNull();
                if (an || bn) {
                    if (an && bn) continue;
                    bool nullsFirst = (k.nulls == NullsOrder::NULLS_FIRST) ||
                                      (k.nulls == NullsOrder::UNSPECIFIED &&
                                       k.order == SortOrder::ASC);
                    // Decide ordering of the null relative to the non-null.
                    bool aBeforeB = an ? nullsFirst : !nullsFirst;
                    return aBeforeB;
                }

                int cmp;
                if (va < vb) cmp = -1; else if (vb < va) cmp = 1; else cmp = 0;
                if (cmp == 0) continue;
                bool asc = (k.order == SortOrder::ASC);
                return asc ? (cmp < 0) : (cmp > 0);
            }
            return false;  // equal on all keys
        });
    built_ = true;
}

Batch Sort::next() {
    if (!built_) build();
    Batch out(child_->schema());
    if (cursor_ >= rows_.size()) return out;

    const size_t end = std::min(cursor_ + kDefaultBatchSize, rows_.size());
    for (size_t i = cursor_; i < end; ++i)
        out.appendRow(rows_[i]);
    cursor_ = end;
    return out;
}

// ============================================================================
// Distinct
// ============================================================================

Distinct::Distinct(OperatorPtr child) : child_(std::move(child)) {}

namespace {
// A stable key for a Row usable in a hash set (NULL-aware).
std::string rowKey(const Row& row) {
    std::string key;
    for (const auto& v : row) {
        if (v.isNull()) { key += "\x01N"; }
        else { key += "\x01"; key += std::to_string(static_cast<int>(v.typeId())); key += ':'; key += v.toString(); }
    }
    return key;
}
}  // namespace

void Distinct::build() {
    std::unordered_map<std::string, bool> seen;
    for (;;) {
        Batch in = child_->next();
        if (in.empty()) break;
        for (size_t r = 0; r < in.numRows; ++r) {
            Row row = in.getRow(r);
            std::string k = rowKey(row);
            if (seen.emplace(k, true).second)
                rows_.push_back(std::move(row));
        }
    }
    built_ = true;
}

Batch Distinct::next() {
    if (!built_) build();
    Batch out(child_->schema());
    if (cursor_ >= rows_.size()) return out;
    const size_t end = std::min(cursor_ + kDefaultBatchSize, rows_.size());
    for (size_t i = cursor_; i < end; ++i) out.appendRow(rows_[i]);
    cursor_ = end;
    return out;
}

// ============================================================================
// HashAggregate
// ============================================================================

HashAggregate::HashAggregate(OperatorPtr child,
                             std::vector<const ast::ExprNode*> groupKeys,
                             std::vector<std::string>          groupKeyNames,
                             std::vector<Agg>                  aggs)
    : child_(std::move(child)), groupKeys_(std::move(groupKeys)), aggs_(std::move(aggs)) {
    // Output schema: group key columns, then aggregate result columns.
    for (size_t i = 0; i < groupKeyNames.size(); ++i) {
        // Key column type is inferred from the child schema where possible;
        // default to FLOAT64 which the evaluator can always hold.
        schema_.addColumn(groupKeyNames[i], DataType::float64().asNullable());
    }
    for (const auto& a : aggs_)
        schema_.addColumn(a.outName, a.outType);
}

void HashAggregate::build() {
    const Schema& sch = child_->schema();
    const size_t nagg = aggs_.size();

    auto newAcc = [&](const Row& keyVals) {
        AccState st;
        st.numAcc.assign(nagg, 0.0);
        st.counts.assign(nagg, 0);
        st.minmax.assign(nagg, Value::null());
        st.seen.assign(nagg, {});
        st.keyValues = keyVals;
        return st;
    };

    bool anyRows = false;
    for (;;) {
        Batch in = child_->next();
        if (in.empty()) break;
        for (size_t r = 0; r < in.numRows; ++r) {
            anyRows = true;
            Row row = in.getRow(r);

            // Compute group key
            Row keyVals;
            keyVals.reserve(groupKeys_.size());
            for (auto* ke : groupKeys_)
                keyVals.push_back(evaluate(*ke, sch, row));
            std::string gk = rowKey(keyVals);

            auto it = groups_.find(gk);
            if (it == groups_.end()) {
                it = groups_.emplace(gk, newAcc(keyVals)).first;
                order_.push_back(gk);
            }
            AccState& st = it->second;

            for (size_t i = 0; i < nagg; ++i) {
                const Agg& a = aggs_[i];
                if (a.star) {                      // COUNT(*)
                    st.counts[i]++;
                    continue;
                }
                Value v = evaluate(*a.arg, sch, row);
                if (v.isNull()) continue;          // aggregates ignore NULLs

                if (a.distinct) {
                    std::string dk = std::to_string((int)v.typeId()) + ":" + v.toString();
                    if (!st.seen[i].emplace(dk, true).second) continue;
                }

                st.counts[i]++;
                if (a.func == "SUM" || a.func == "AVG") {
                    st.numAcc[i] += v.toDouble();
                } else if (a.func == "MIN") {
                    if (st.minmax[i].isNull() || v < st.minmax[i]) st.minmax[i] = v;
                } else if (a.func == "MAX") {
                    if (st.minmax[i].isNull() || st.minmax[i] < v) st.minmax[i] = v;
                }
                // COUNT: counts[i] already incremented
            }
        }
    }

    // Special case: aggregate with no GROUP BY and no input rows still yields
    // one output row (COUNT → 0, others → NULL).
    if (groupKeys_.empty() && !anyRows) {
        AccState st = newAcc(Row{});
        std::string gk = rowKey(Row{});
        groups_.emplace(gk, std::move(st));
        order_.push_back(gk);
    }

    built_ = true;
}

Batch HashAggregate::next() {
    if (!built_) build();
    Batch out(schema_);
    if (cursor_ >= order_.size()) return out;

    const size_t end = std::min(cursor_ + kDefaultBatchSize, order_.size());
    for (size_t gi = cursor_; gi < end; ++gi) {
        const AccState& st = groups_.at(order_[gi]);
        Row outRow;
        outRow.reserve(groupKeys_.size() + aggs_.size());

        for (const auto& kv : st.keyValues) outRow.push_back(kv);

        for (size_t i = 0; i < aggs_.size(); ++i) {
            const Agg& a = aggs_[i];
            if (a.func == "COUNT") {
                outRow.push_back(Value(static_cast<int64_t>(st.counts[i])));
            } else if (a.func == "SUM") {
                outRow.push_back(st.counts[i] == 0 ? Value::null() : Value(st.numAcc[i]));
            } else if (a.func == "AVG") {
                outRow.push_back(st.counts[i] == 0 ? Value::null()
                                                   : Value(st.numAcc[i] / st.counts[i]));
            } else if (a.func == "MIN" || a.func == "MAX") {
                outRow.push_back(st.minmax[i]);
            } else {
                outRow.push_back(Value::null());
            }
        }
        out.appendRow(std::move(outRow));
    }
    cursor_ = end;
    return out;
}

// ============================================================================
// HashJoin
// ============================================================================

HashJoin::HashJoin(OperatorPtr left, OperatorPtr right,
                   ast::JoinType type, const ast::ExprNode* condition,
                   Schema outputSchema)
    : left_(std::move(left)), right_(std::move(right)),
      type_(type), condition_(condition), schema_(std::move(outputSchema)) {
    leftSchema_  = left_->schema();
    rightSchema_ = right_->schema();
}

void HashJoin::reset() {
    built_ = false; cursor_ = 0;
    leftRows_.clear(); rightRows_.clear(); rightMatched_.clear(); result_.clear();
    left_->reset(); right_->reset();
}

void HashJoin::materializeLeft() {
    for (;;) {
        Batch in = left_->next();
        if (in.empty()) break;
        for (size_t r = 0; r < in.numRows; ++r) leftRows_.push_back(in.getRow(r));
    }
    for (;;) {
        Batch in = right_->next();
        if (in.empty()) break;
        for (size_t r = 0; r < in.numRows; ++r) rightRows_.push_back(in.getRow(r));
    }
    rightMatched_.assign(rightRows_.size(), 0);
}

void HashJoin::computeResult() {
    materializeLeft();

    const size_t lcols = leftSchema_.size();
    const size_t rcols = rightSchema_.size();

    // Merged schema used to evaluate the ON condition over concatenated rows.
    Schema merged = Schema::merge(leftSchema_, rightSchema_);

    auto nullRow = [](size_t n) { return Row(n, Value::null()); };

    auto emit = [&](const Row& l, const Row& r) {
        Row out;
        out.reserve(lcols + rcols);
        out.insert(out.end(), l.begin(), l.end());
        out.insert(out.end(), r.begin(), r.end());
        result_.push_back(std::move(out));
    };

    for (size_t li = 0; li < leftRows_.size(); ++li) {
        bool leftMatched = false;
        for (size_t ri = 0; ri < rightRows_.size(); ++ri) {
            bool ok = true;
            if (type_ != JoinType::CROSS && condition_) {
                Row combined;
                combined.reserve(lcols + rcols);
                combined.insert(combined.end(), leftRows_[li].begin(), leftRows_[li].end());
                combined.insert(combined.end(), rightRows_[ri].begin(), rightRows_[ri].end());
                ok = evaluatePredicate(*condition_, merged, combined);
            }
            if (ok) {
                emit(leftRows_[li], rightRows_[ri]);
                leftMatched = true;
                rightMatched_[ri] = 1;
            }
        }
        // LEFT / FULL: unmatched left row + NULL-extended right
        if (!leftMatched &&
            (type_ == JoinType::LEFT || type_ == JoinType::FULL)) {
            emit(leftRows_[li], nullRow(rcols));
        }
    }

    // RIGHT / FULL: unmatched right rows + NULL-extended left
    if (type_ == JoinType::RIGHT || type_ == JoinType::FULL) {
        for (size_t ri = 0; ri < rightRows_.size(); ++ri)
            if (!rightMatched_[ri])
                emit(nullRow(lcols), rightRows_[ri]);
    }

    built_ = true;
}

Batch HashJoin::next() {
    if (!built_) computeResult();
    Batch out(schema_);
    if (cursor_ >= result_.size()) return out;
    const size_t end = std::min(cursor_ + kDefaultBatchSize, result_.size());
    for (size_t i = cursor_; i < end; ++i) out.appendRow(result_[i]);
    cursor_ = end;
    return out;
}

// ============================================================================
// Append
// ============================================================================

Append::Append(OperatorPtr left, OperatorPtr right)
    : left_(std::move(left)), right_(std::move(right)) {}

Batch Append::next() {
    if (!leftDone_) {
        Batch b = left_->next();
        if (!b.empty()) return b;
        leftDone_ = true;
    }
    // Right side: re-tag rows with the left (output) schema positionally.
    Batch in = right_->next();
    if (in.empty()) return Batch(left_->schema());
    Batch out(left_->schema());
    for (size_t r = 0; r < in.numRows; ++r)
        out.appendRow(in.getRow(r));
    return out;
}

// ============================================================================
// Values
// ============================================================================

Values::Values(Schema schema, std::vector<Row> rows)
    : schema_(std::move(schema)), rows_(std::move(rows)) {}

Batch Values::next() {
    Batch out(schema_);
    if (cursor_ >= rows_.size()) return out;
    const size_t end = std::min(cursor_ + kDefaultBatchSize, rows_.size());
    for (size_t i = cursor_; i < end; ++i) out.appendRow(rows_[i]);
    cursor_ = end;
    return out;
}

// ============================================================================
// drain
// ============================================================================

Batch drain(Operator& root) {
    Batch all(root.schema());
    for (;;) {
        Batch b = root.next();
        if (b.empty()) break;
        for (size_t r = 0; r < b.numRows; ++r)
            all.appendRow(b.getRow(r));
    }
    return all;
}

}  // namespace exec
}  // namespace lq
