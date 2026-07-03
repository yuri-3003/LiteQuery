// ============================================================================
// LiteQuery — fast_aggregate.cpp
// Typed vectorized execution for SELECT [key,] AGG(col)... GROUP BY [key].
// ============================================================================

#include "fast_aggregate.h"

#include "catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace lq {

using namespace ast;

namespace {

// ---- Predicate: a conjunction of  (column <cmp> constant)  atoms -----------

enum class Cmp { EQ, NEQ, LT, LTE, GT, GTE };

struct PredAtom {
    int   col;        // column index in the table
    Cmp   op;
    double  numConst; // comparison constant (numeric columns)
    bool    isNumeric;
    std::string strConst;
};

std::optional<Cmp> toCmp(BinaryOp op) {
    switch (op) {
        case BinaryOp::EQ:  return Cmp::EQ;
        case BinaryOp::NEQ: return Cmp::NEQ;
        case BinaryOp::LT:  return Cmp::LT;
        case BinaryOp::LTE: return Cmp::LTE;
        case BinaryOp::GT:  return Cmp::GT;
        case BinaryOp::GTE: return Cmp::GTE;
        default: return std::nullopt;
    }
}

Cmp flip(Cmp c) {   // when operands are swapped (const <cmp> col)
    switch (c) {
        case Cmp::LT:  return Cmp::GT;
        case Cmp::LTE: return Cmp::GTE;
        case Cmp::GT:  return Cmp::LT;
        case Cmp::GTE: return Cmp::LTE;
        default:       return c;   // EQ/NEQ symmetric
    }
}

// Try to read a "column <cmp> constant" atom (either operand order).
bool parseAtom(const ExprNode& e, const Table& t, PredAtom& out) {
    const auto* bin = std::get_if<BinaryExpr>(&e);
    if (!bin) return false;
    auto cmp = toCmp(bin->op);
    if (!cmp) return false;

    const ColumnRef* col = nullptr;
    const Literal*   lit = nullptr;
    bool swapped = false;

    if ((col = std::get_if<ColumnRef>(bin->left.get())) &&
        (lit = std::get_if<Literal>(bin->right.get()))) {
        // col <cmp> const
    } else if ((col = std::get_if<ColumnRef>(bin->right.get())) &&
               (lit = std::get_if<Literal>(bin->left.get()))) {
        swapped = true;
    } else {
        return false;
    }

    int idx = t.schema().indexOf(col->column);
    if (idx < 0) return false;

    out.col = idx;
    out.op  = swapped ? flip(*cmp) : *cmp;

    const Value& v = lit->value;
    if (v.isNull()) return false;
    if (v.typeId() == TypeId::VARCHAR) {
        out.isNumeric = false;
        out.strConst  = v.getString();
    } else if (typeIsNumeric(v.typeId())) {
        out.isNumeric = true;
        out.numConst  = v.toDouble();
    } else {
        return false;
    }
    return true;
}

// Flatten an AND-tree of atoms; returns false if any leaf isn't a simple atom.
bool parsePredicate(const ExprNode& e, const Table& t, std::vector<PredAtom>& out) {
    if (const auto* bin = std::get_if<BinaryExpr>(&e)) {
        if (bin->op == BinaryOp::AND)
            return parsePredicate(*bin->left, t, out) && parsePredicate(*bin->right, t, out);
    }
    PredAtom a;
    if (!parseAtom(e, t, a)) return false;
    out.push_back(std::move(a));
    return true;
}

template <typename T>
bool applyCmp(Cmp op, T lhs, T rhs) {
    switch (op) {
        case Cmp::EQ:  return lhs == rhs;
        case Cmp::NEQ: return lhs != rhs;
        case Cmp::LT:  return lhs <  rhs;
        case Cmp::LTE: return lhs <= rhs;
        case Cmp::GT:  return lhs >  rhs;
        case Cmp::GTE: return lhs >= rhs;
    }
    return false;
}

// Evaluate all atoms for row r (AND). NULL in any compared cell → row excluded.
bool rowPasses(const Table& t, const std::vector<PredAtom>& preds, size_t r) {
    for (const auto& p : preds) {
        const Column& col = t.columnAt(p.col);
        if (!col.isValid(r)) return false;
        if (p.isNumeric) {
            double v = (col.kind() == Column::Kind::Double)
                           ? col.f64()[r]
                           : static_cast<double>(col.i64()[r]);
            if (!applyCmp(p.op, v, p.numConst)) return false;
        } else {
            if (col.kind() != Column::Kind::String) return false;
            if (!applyCmp<std::string>(p.op, col.str()[r], p.strConst)) return false;
        }
    }
    return true;
}

// ---- Aggregate spec --------------------------------------------------------

enum class AggFn { Count, Sum, Avg, Min, Max };

struct AggSpec {
    AggFn       fn;
    bool        star;      // COUNT(*)
    int         col;       // argument column (-1 for COUNT(*))
    std::string outName;
    DataType    outType;
};

struct AggState {
    int64_t count = 0;     // non-null argument count (or rows for COUNT(*))
    double  sum   = 0.0;
    double  mn    = std::numeric_limits<double>::infinity();
    double  mx    = -std::numeric_limits<double>::infinity();
};

// One group's accumulators + its key value.
struct Group {
    std::vector<AggState> aggs;
    Value                 key;   // NULL for the no-GROUP-BY single group
};

}  // namespace

std::optional<QueryResult> tryFastAggregate(const SelectStmt& stmt, Catalog& catalog) {
    // ---- Shape gate --------------------------------------------------------
    if (stmt.setOp || stmt.distinct) return std::nullopt;
    if (!stmt.from || stmt.from->tables.size() != 1) return std::nullopt;
    if (stmt.having) return std::nullopt;                    // keep it simple

    // FROM must be a single, unaliased base table.
    const auto* tref = std::get_if<TableRef>(stmt.from->tables[0].get());
    if (!tref) return std::nullopt;
    if (!catalog.hasTable(tref->name)) return std::nullopt;
    TablePtr table = catalog.getTable(tref->name);
    const Table& t = *table;

    // GROUP BY: zero or one column reference.
    int groupCol = -1;
    std::string groupName;
    if (stmt.groupBy) {
        if (stmt.groupBy->keys.size() != 1) return std::nullopt;
        const auto* gc = std::get_if<ColumnRef>(stmt.groupBy->keys[0].get());
        if (!gc) return std::nullopt;
        groupCol = t.schema().indexOf(gc->column);
        if (groupCol < 0) return std::nullopt;
        groupName = gc->column;
    }

    // SELECT list: an optional leading group-key column, then aggregates only.
    std::vector<AggSpec> aggs;
    bool selectHasKey = false;
    for (const auto& item : stmt.selectList) {
        if (const auto* cr = std::get_if<ColumnRef>(item.expr.get())) {
            // Only the group-by column may appear bare in the SELECT list.
            if (groupCol < 0) return std::nullopt;
            if (cr->column != groupName) return std::nullopt;
            selectHasKey = true;
            continue;
        }
        const auto* fc = std::get_if<FunctionCall>(item.expr.get());
        if (!fc || !fc->isAggregate()) return std::nullopt;

        AggSpec spec;
        spec.star = fc->star;
        spec.col  = -1;
        if      (fc->name == "COUNT") spec.fn = AggFn::Count;
        else if (fc->name == "SUM")   spec.fn = AggFn::Sum;
        else if (fc->name == "AVG")   spec.fn = AggFn::Avg;
        else if (fc->name == "MIN")   spec.fn = AggFn::Min;
        else if (fc->name == "MAX")   spec.fn = AggFn::Max;
        else return std::nullopt;

        if (!fc->star) {
            if (fc->distinct) return std::nullopt;           // COUNT(DISTINCT) → general path
            if (fc->args.size() != 1) return std::nullopt;
            const auto* ac = std::get_if<ColumnRef>(fc->args[0].get());
            if (!ac) return std::nullopt;                    // only AGG(column)
            spec.col = t.schema().indexOf(ac->column);
            if (spec.col < 0) return std::nullopt;
            // MIN/MAX/SUM/AVG operate on numeric columns here.
            if (t.columnAt(spec.col).kind() == Column::Kind::String) return std::nullopt;
        }
        spec.outName = item.alias ? *item.alias : fc->name;
        spec.outType = (fc->name == "COUNT") ? DataType::int64() : DataType::float64().asNullable();
        aggs.push_back(std::move(spec));
    }
    if (aggs.empty()) return std::nullopt;                   // must have ≥1 aggregate

    // ORDER BY / LIMIT: allow, but only order by the group key or an aggregate
    // output name; otherwise fall back so semantics stay correct.
    // (Applied after aggregation below.)

    // ---- WHERE -------------------------------------------------------------
    std::vector<PredAtom> preds;
    if (stmt.where) {
        if (!parsePredicate(*stmt.where->predicate, t, preds))
            return std::nullopt;                             // complex predicate → general path
    }

    // ---- Execute over typed columns ---------------------------------------
    const size_t nrows = t.rowCount();
    const size_t nagg  = aggs.size();

    std::vector<Group> groups;
    std::unordered_map<std::string, size_t> groupIndex;   // key string → group slot

    auto groupSlot = [&](size_t r) -> Group& {
        if (groupCol < 0) {
            if (groups.empty()) { groups.push_back({std::vector<AggState>(nagg), Value::null()}); }
            return groups[0];
        }
        const Column& gcol = t.columnAt(groupCol);
        // Build a stable key string for the group value.
        std::string k;
        Value keyVal;
        if (!gcol.isValid(r)) { k = "\x00N"; keyVal = Value::null(); }
        else if (gcol.kind() == Column::Kind::String) { k = "S" + gcol.str()[r]; keyVal = Value(gcol.str()[r]); }
        else if (gcol.kind() == Column::Kind::Double)  { double d = gcol.f64()[r]; k = "D" + std::to_string(d); keyVal = Value(d); }
        else { int64_t v = gcol.i64()[r]; k = "I" + std::to_string(v); keyVal = gcol.valueAt(r); }

        auto it = groupIndex.find(k);
        if (it == groupIndex.end()) {
            size_t slot = groups.size();
            groups.push_back({std::vector<AggState>(nagg), std::move(keyVal)});
            groupIndex.emplace(std::move(k), slot);
            return groups[slot];
        }
        return groups[it->second];
    };

    for (size_t r = 0; r < nrows; ++r) {
        if (!preds.empty() && !rowPasses(t, preds, r)) continue;
        Group& g = groupSlot(r);
        for (size_t i = 0; i < nagg; ++i) {
            const AggSpec& a = aggs[i];
            AggState& st = g.aggs[i];
            if (a.star) { st.count++; continue; }
            const Column& col = t.columnAt(a.col);
            if (!col.isValid(r)) continue;                   // aggregates skip NULLs
            double v = (col.kind() == Column::Kind::Double)
                           ? col.f64()[r]
                           : static_cast<double>(col.i64()[r]);
            st.count++;
            switch (a.fn) {
                case AggFn::Count: break;                    // count only
                case AggFn::Sum: case AggFn::Avg: st.sum += v; break;
                case AggFn::Min: if (v < st.mn) st.mn = v; break;
                case AggFn::Max: if (v > st.mx) st.mx = v; break;
            }
        }
    }

    // No GROUP BY and no rows still yields one output row.
    if (groupCol < 0 && groups.empty())
        groups.push_back({std::vector<AggState>(nagg), Value::null()});

    // ---- Assemble the result set ------------------------------------------
    QueryResult res;
    if (selectHasKey) res.schema.addColumn(groupName, t.columnAt(groupCol).type());
    for (const auto& a : aggs) res.schema.addColumn(a.outName, a.outType);

    for (const auto& g : groups) {
        Row row;
        if (selectHasKey) row.push_back(g.key);
        for (size_t i = 0; i < nagg; ++i) {
            const AggSpec& a = aggs[i];
            const AggState& st = g.aggs[i];
            switch (a.fn) {
                case AggFn::Count: row.push_back(Value(static_cast<int64_t>(st.count))); break;
                case AggFn::Sum:   row.push_back(st.count == 0 ? Value::null() : Value(st.sum)); break;
                case AggFn::Avg:   row.push_back(st.count == 0 ? Value::null() : Value(st.sum / st.count)); break;
                case AggFn::Min:   row.push_back(st.count == 0 ? Value::null() : Value(st.mn)); break;
                case AggFn::Max:   row.push_back(st.count == 0 ? Value::null() : Value(st.mx)); break;
            }
        }
        res.rows.push_back(std::move(row));
    }

    // ---- ORDER BY / LIMIT (over the small result set) ---------------------
    if (stmt.orderBy) {
        // Resolve each key to a result-column index; bail to the general path
        // if any key doesn't name an output column (keeps semantics correct).
        struct SortKey { int col; bool asc; };
        std::vector<SortKey> keys;
        for (const auto& k : stmt.orderBy->keys) {
            const auto* cr = std::get_if<ColumnRef>(k.expr.get());
            if (!cr) return std::nullopt;
            int idx = res.schema.indexOf(cr->column);
            if (idx < 0) return std::nullopt;
            keys.push_back({idx, k.order == SortOrder::ASC});
        }
        std::stable_sort(res.rows.begin(), res.rows.end(),
            [&](const Row& a, const Row& b) {
                for (const auto& sk : keys) {
                    const Value& va = a[sk.col];
                    const Value& vb = b[sk.col];
                    if (va.isNull() || vb.isNull()) {
                        if (va.isNull() && vb.isNull()) continue;
                        return sk.asc ? va.isNull() : vb.isNull();
                    }
                    bool lt = va < vb, gt = vb < va;
                    if (!lt && !gt) continue;
                    return sk.asc ? lt : gt;
                }
                return false;
            });
    }
    if (stmt.limit) {
        int64_t off = 0, lim = -1;
        if (stmt.limit->offset) {
            const auto* l = std::get_if<Literal>(stmt.limit->offset->get());
            if (!l) return std::nullopt;
            off = l->value.toInt64();
        }
        if (stmt.limit->limit) {
            const auto* l = std::get_if<Literal>(stmt.limit->limit->get());
            if (!l) return std::nullopt;
            lim = l->value.toInt64();
        }
        if (off > 0) {
            if ((size_t)off >= res.rows.size()) res.rows.clear();
            else res.rows.erase(res.rows.begin(), res.rows.begin() + off);
        }
        if (lim >= 0 && (size_t)lim < res.rows.size())
            res.rows.resize(lim);
    }

    return res;
}

}  // namespace lq
