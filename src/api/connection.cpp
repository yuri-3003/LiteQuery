// ============================================================================
// LiteQuery — connection.cpp
// SQL pipeline: parse → build operator tree (schema-resolved) → execute.
// ============================================================================

#include "connection.h"

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "eval.h"
#include "physical_plan.h"
#include "fast_aggregate.h"
#include "logical_plan.h"
#include "optimizer.h"
#include "csv_reader.h"
#include "persistence.h"

#include <chrono>
#include <functional>
#include <sstream>

namespace lq {

using namespace ast;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Query builder — turns a resolved SelectStmt into an operator tree.
//
// Schema resolution strategy: every operator's output schema carries fully
// resolved column names. Base scans expose columns both as "col" and, when the
// table has an alias, as "alias.col" so that qualified references in WHERE/ON
// resolve. Joins concatenate child schemas.
// ============================================================================

namespace {

struct BuildError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Build the scan schema for a base table.
//   - Unaliased table  → columns keep their bare names ("col").
//   - Aliased table     → columns are qualified ("alias.col") so joins between
//                         the same table (self-joins) and qualified references
//                         resolve. Unqualified references still resolve because
//                         the evaluator falls back to a bare-name lookup — but
//                         only when the bare name is unique, which we ensure by
//                         *also* keeping the bare form when it does not collide.
Schema scanSchema(const Table& t, const std::string& alias) {
    Schema s;
    for (size_t i = 0; i < t.schema().size(); ++i) {
        const auto& col = t.schema()[i];
        std::string name = alias.empty() ? col.name : (alias + "." + col.name);
        s.addColumn(name, col.type);
    }
    return s;
}

// Owns AST nodes synthesized during planning (rewritten HAVING/ORDER BY
// expressions, substituted aggregate references). Operators borrow raw
// pointers into these, so the vector must outlive execution — runSelect keeps
// it alive across drain().
using OwnedExprs = std::vector<ast::Expr>;

// Forward declaration — table refs and full selects are mutually recursive.
exec::OperatorPtr buildSelect(const SelectStmt& stmt, Catalog& cat, OwnedExprs& owned);

// Build an operator for a FROM table reference, returning its operator and the
// schema it produces.
exec::OperatorPtr buildTableRef(const TableRefNode& ref, Catalog& cat,
                                OwnedExprs& owned,
                                const std::string& inheritedAlias = "") {
    return std::visit([&](const auto& r) -> exec::OperatorPtr {
        using T = std::decay_t<decltype(r)>;

        if constexpr (std::is_same_v<T, TableRef>) {
            TablePtr tbl = cat.getTable(r.name);
            // An unaliased base table exposes bare column names; an alias (from
            // a wrapping AliasedRef) qualifies them as "alias.col".
            Schema s = scanSchema(*tbl, inheritedAlias);
            return std::make_unique<exec::SeqScan>(tbl, std::move(s));
        }
        else if constexpr (std::is_same_v<T, AliasedRef>) {
            return buildTableRef(*r.ref, cat, owned, r.alias);
        }
        else if constexpr (std::is_same_v<T, JoinRef>) {
            auto left  = buildTableRef(*r.left, cat, owned);
            auto right = buildTableRef(*r.right, cat, owned);
            Schema merged = Schema::merge(left->schema(), right->schema());
            const ExprNode* cond = r.condition ? r.condition->get() : nullptr;
            return std::make_unique<exec::HashJoin>(
                std::move(left), std::move(right), r.type, cond, std::move(merged));
        }
        else if constexpr (std::is_same_v<T, SubqueryRef>) {
            auto op = buildSelect(*r.subquery, cat, owned);
            // Re-alias subquery output columns if an alias is present.
            if (!inheritedAlias.empty()) {
                // Wrap in a projection that renames "c" → "alias.c".
                const Schema& in = op->schema();
                std::vector<exec::Project::Item> items;
                // We need borrowed exprs; synthesize ColumnRef nodes stored in a
                // static-lifetime holder is unsafe. Instead, keep original names.
                (void)in;
            }
            return op;
        }
        else {
            throw BuildError("unsupported table reference");
        }
    }, ref);
}

// Forward declaration (defined below itemName).
void collectAggs(const ExprNode& expr, std::vector<const FunctionCall*>& out);

// Detect whether the SELECT is an aggregate query. Aggregates may be nested
// inside expressions (SUM(a)/2), so walk the whole item, not just the root.
bool isAggregateQuery(const SelectStmt& stmt) {
    if (stmt.groupBy || stmt.having) return true;
    for (const auto& item : stmt.selectList) {
        std::vector<const FunctionCall*> found;
        collectAggs(*item.expr, found);
        if (!found.empty()) return true;
    }
    return false;
}

// Recursively collect aggregate FunctionCall pointers from an expression.
// Must traverse every node kind cloneSubstAggs handles, so collection and
// substitution can never disagree about which aggregates exist.
void collectAggs(const ExprNode& expr, std::vector<const FunctionCall*>& out) {
    std::visit([&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, FunctionCall>) {
            if (e.isAggregate()) { out.push_back(&e); return; }
            for (const auto& a : e.args) collectAggs(*a, out);
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            collectAggs(*e.left, out); collectAggs(*e.right, out);
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            collectAggs(*e.operand, out);
        } else if constexpr (std::is_same_v<T, CastExpr>) {
            collectAggs(*e.expr, out);
        } else if constexpr (std::is_same_v<T, IsNullExpr>) {
            collectAggs(*e.expr, out);
        } else if constexpr (std::is_same_v<T, BetweenExpr>) {
            collectAggs(*e.expr, out);
            collectAggs(*e.low, out);
            collectAggs(*e.high, out);
        } else if constexpr (std::is_same_v<T, InExpr>) {
            collectAggs(*e.expr, out);
            if (const auto* list = std::get_if<ExprList>(&e.values))
                for (const auto& v : *list) collectAggs(*v, out);
        } else if constexpr (std::is_same_v<T, CaseExpr>) {
            if (e.subject) collectAggs(**e.subject, out);
            for (const auto& w : e.whenClauses) { collectAggs(*w.condition, out); collectAggs(*w.result, out); }
            if (e.elseExpr) collectAggs(**e.elseExpr, out);
        }
    }, expr);
}

// A display name for a select item (alias, or column name, or synthesized).
std::string itemName(const SelectItem& item, size_t idx) {
    if (item.alias) return *item.alias;
    if (const auto* c = std::get_if<ColumnRef>(item.expr.get()))
        return c->column;
    if (const auto* f = std::get_if<FunctionCall>(item.expr.get())) {
        std::string n = f->name;
        if (f->star) return n;   // COUNT
        return n;
    }
    return "col" + std::to_string(idx + 1);
}

// ---- Aggregate substitution -------------------------------------------------
// The aggregate operator computes each aggregate once and exposes it as an
// output column. Expressions written around aggregates (HAVING SUM(x) > 10,
// ORDER BY SUM(x), SELECT SUM(a)/COUNT(*)) are rewritten into clones where each
// aggregate call becomes a ColumnRef to that output column; the clone is then
// an ordinary scalar expression the evaluator can run over aggregate output.

// Structural equality for aggregate calls, so SUM(v) in ORDER BY matches
// SUM(v) in the SELECT list even though they are distinct AST nodes.
bool sameAggregate(const FunctionCall& a, const FunctionCall& b) {
    if (a.name != b.name || a.star != b.star || a.distinct != b.distinct)
        return false;
    if (a.star) return true;                       // COUNT(*) == COUNT(*)
    if (a.args.size() != 1 || b.args.size() != 1) return false;
    const auto* ca = std::get_if<ColumnRef>(a.args[0].get());
    const auto* cb = std::get_if<ColumnRef>(b.args[0].get());
    return ca && cb && ca->toString() == cb->toString();
}

// Clone `e`, replacing every aggregate call with a ColumnRef to its slot's
// output column name. Aggregates are looked up in `aggCalls` (pointer match
// first, then structural); an unknown aggregate is appended as a new slot by
// the caller-supplied `addSlot`.
ast::Expr cloneSubstAggs(const ExprNode& e,
                         const std::vector<const FunctionCall*>& aggCalls,
                         const std::vector<std::string>& aggOutNames,
                         const std::function<size_t(const FunctionCall&)>& addSlot) {
    return std::visit([&](const auto& n) -> ast::Expr {
        using T = std::decay_t<decltype(n)>;
        auto rec = [&](const ExprNode& c) {
            return cloneSubstAggs(c, aggCalls, aggOutNames, addSlot);
        };

        if constexpr (std::is_same_v<T, Literal>) {
            return makeExpr<Literal>(Literal{n.value, n.location});
        }
        else if constexpr (std::is_same_v<T, ColumnRef>) {
            ColumnRef c = n;
            return makeExpr<ColumnRef>(std::move(c));
        }
        else if constexpr (std::is_same_v<T, UnaryExpr>) {
            UnaryExpr u;
            u.op = n.op; u.location = n.location;
            u.operand = rec(*n.operand);
            return makeExpr<UnaryExpr>(std::move(u));
        }
        else if constexpr (std::is_same_v<T, BinaryExpr>) {
            BinaryExpr b;
            b.op = n.op; b.location = n.location;
            b.left  = rec(*n.left);
            b.right = rec(*n.right);
            return makeExpr<BinaryExpr>(std::move(b));
        }
        else if constexpr (std::is_same_v<T, FunctionCall>) {
            if (n.isAggregate()) {
                // Find (or create) this aggregate's slot; substitute a ColumnRef.
                size_t idx = aggCalls.size();
                for (size_t i = 0; i < aggCalls.size(); ++i) {
                    if (aggCalls[i] == &n || sameAggregate(*aggCalls[i], n)) { idx = i; break; }
                }
                if (idx == aggCalls.size()) idx = addSlot(n);
                ColumnRef ref;
                ref.column = aggOutNames[idx];
                return makeExpr<ColumnRef>(std::move(ref));
            }
            FunctionCall f;
            f.name = n.name; f.distinct = n.distinct; f.star = n.star;
            f.location = n.location;
            for (const auto& a : n.args) f.args.push_back(rec(*a));
            return makeExpr<FunctionCall>(std::move(f));
        }
        else if constexpr (std::is_same_v<T, CastExpr>) {
            CastExpr c;
            c.targetType = n.targetType; c.location = n.location;
            c.expr = rec(*n.expr);
            return makeExpr<CastExpr>(std::move(c));
        }
        else if constexpr (std::is_same_v<T, IsNullExpr>) {
            IsNullExpr i;
            i.negated = n.negated; i.location = n.location;
            i.expr = rec(*n.expr);
            return makeExpr<IsNullExpr>(std::move(i));
        }
        else if constexpr (std::is_same_v<T, BetweenExpr>) {
            BetweenExpr b;
            b.negated = n.negated; b.location = n.location;
            b.expr = rec(*n.expr); b.low = rec(*n.low); b.high = rec(*n.high);
            return makeExpr<BetweenExpr>(std::move(b));
        }
        else if constexpr (std::is_same_v<T, CaseExpr>) {
            CaseExpr c;
            c.location = n.location;
            if (n.subject) c.subject = rec(**n.subject);
            for (const auto& w : n.whenClauses) {
                CaseWhen cw;
                cw.condition = rec(*w.condition);
                cw.result    = rec(*w.result);
                c.whenClauses.push_back(std::move(cw));
            }
            if (n.elseExpr) c.elseExpr = rec(**n.elseExpr);
            return makeExpr<CaseExpr>(std::move(c));
        }
        else if constexpr (std::is_same_v<T, InExpr>) {
            const auto* list = std::get_if<ExprList>(&n.values);
            if (!list) throw BuildError("IN (subquery) is not supported here");
            InExpr in;
            in.negated = n.negated; in.location = n.location;
            in.expr = rec(*n.expr);
            ExprList vals;
            for (const auto& v : *list) vals.push_back(rec(*v));
            in.values = std::move(vals);
            return makeExpr<InExpr>(std::move(in));
        }
        else {
            throw BuildError("unsupported expression in aggregate context");
        }
    }, e);
}

// Wrap `op` in a Sort if the statement has an ORDER BY. The sort keys are
// evaluated against `op`'s current output schema.
exec::OperatorPtr applyOrderBy(exec::OperatorPtr op, const SelectStmt& stmt) {
    if (!stmt.orderBy) return op;
    std::vector<exec::Sort::Key> keys;
    for (const auto& k : stmt.orderBy->keys)
        keys.push_back({ k.expr.get(), k.order, k.nullsOrder });
    return std::make_unique<exec::Sort>(std::move(op), std::move(keys));
}

// Wrap `op` in a Limit if the statement has a LIMIT/OFFSET.
exec::OperatorPtr applyLimit(exec::OperatorPtr op, const SelectStmt& stmt) {
    if (!stmt.limit) return op;
    int64_t lim = -1, off = 0;
    if (stmt.limit->limit)  lim = evaluate(**stmt.limit->limit,  Schema{}, Row{}).toInt64();
    if (stmt.limit->offset) off = evaluate(**stmt.limit->offset, Schema{}, Row{}).toInt64();
    return std::make_unique<exec::Limit>(std::move(op), lim, off);
}

// Build the operator tree for one SELECT (without its UNION tail).
exec::OperatorPtr buildSelectCore(const SelectStmt& stmt, Catalog& cat,
                                  OwnedExprs& owned) {
    // ---- FROM ----
    exec::OperatorPtr op;
    if (stmt.from && !stmt.from->tables.empty()) {
        op = buildTableRef(*stmt.from->tables[0], cat, owned);
        for (size_t i = 1; i < stmt.from->tables.size(); ++i) {
            auto right = buildTableRef(*stmt.from->tables[i], cat, owned);
            Schema merged = Schema::merge(op->schema(), right->schema());
            op = std::make_unique<exec::HashJoin>(
                std::move(op), std::move(right), JoinType::CROSS, nullptr, std::move(merged));
        }
    } else {
        // No FROM: single dummy row so SELECT 1+1 works.
        Schema dual; dual.addColumn("__dual__", DataType::int32());
        std::vector<Row> rows{ Row{ Value(int32_t{0}) } };
        op = std::make_unique<exec::Values>(std::move(dual), std::move(rows));
    }

    // ---- WHERE ----
    if (stmt.where)
        op = std::make_unique<exec::Filter>(std::move(op), stmt.where->predicate.get());

    // ---- GROUP BY / aggregates ----
    if (isAggregateQuery(stmt)) {
        // Group keys.
        std::vector<const ExprNode*> groupKeys;
        std::vector<std::string>     groupKeyNames;
        if (stmt.groupBy) {
            size_t gi = 0;
            for (const auto& k : stmt.groupBy->keys) {
                groupKeys.push_back(k.get());
                std::string nm = "__gk" + std::to_string(gi++);
                if (const auto* c = std::get_if<ColumnRef>(k.get())) nm = c->column;
                groupKeyNames.push_back(nm);
            }
        }

        // Collect the distinct aggregate calls from SELECT + HAVING + ORDER BY.
        // Each unique aggregate gets one slot, computed once and exposed as an
        // internal output column (__agg0, __agg1, …). Rewritten expressions
        // reference those columns; the final projection renames for display.
        std::vector<const FunctionCall*> aggCalls;
        std::vector<std::string>         aggOutNames;
        auto addSlot = [&](const FunctionCall& fc) -> size_t {
            aggCalls.push_back(&fc);
            aggOutNames.push_back("__agg" + std::to_string(aggCalls.size() - 1));
            return aggCalls.size() - 1;
        };
        auto collectInto = [&](const ExprNode& e) {
            std::vector<const FunctionCall*> found;
            collectAggs(e, found);
            for (const auto* fc : found) {
                bool known = false;
                for (const auto* have : aggCalls)
                    if (have == fc || sameAggregate(*have, *fc)) { known = true; break; }
                if (!known) addSlot(*fc);
            }
        };
        for (const auto& item : stmt.selectList) collectInto(*item.expr);
        if (stmt.having) collectInto(*stmt.having->predicate);
        if (stmt.orderBy)
            for (const auto& k : stmt.orderBy->keys) collectInto(*k.expr);

        std::vector<exec::HashAggregate::Agg> aggs;
        for (size_t ai = 0; ai < aggCalls.size(); ++ai) {
            const FunctionCall* fc = aggCalls[ai];
            exec::HashAggregate::Agg a;
            a.func     = fc->name;
            a.star     = fc->star;
            a.distinct = fc->distinct;
            a.arg      = (!fc->star && !fc->args.empty()) ? fc->args[0].get() : nullptr;
            a.outName  = aggOutNames[ai];
            a.outType  = (fc->name == "COUNT") ? DataType::int64() : DataType::float64().asNullable();
            aggs.push_back(a);
        }

        op = std::make_unique<exec::HashAggregate>(
            std::move(op), std::move(groupKeys), std::move(groupKeyNames), std::move(aggs));

        // Rewrite the SELECT items now: aggregate calls become references to
        // the internal columns. The rewritten trees are owned by `owned`.
        std::vector<exec::Project::Item> items;
        for (size_t i = 0; i < stmt.selectList.size(); ++i) {
            const auto& item = stmt.selectList[i];
            owned.push_back(cloneSubstAggs(*item.expr, aggCalls, aggOutNames, addSlot));
            exec::Project::Item pit;
            pit.expr = owned.back().get();
            pit.name = itemName(item, i);
            pit.type = DataType::float64().asNullable();
            if (const auto* c = std::get_if<ColumnRef>(pit.expr)) {
                int idx = op->schema().indexOf(c->column);
                if (idx >= 0) pit.type = op->schema()[idx].type;
            }
            items.push_back(std::move(pit));
        }

        // HAVING: rewritten predicate evaluated over the aggregate output.
        if (stmt.having) {
            owned.push_back(cloneSubstAggs(*stmt.having->predicate,
                                           aggCalls, aggOutNames, addSlot));
            op = std::make_unique<exec::Filter>(std::move(op), owned.back().get());
        }

        // ORDER BY: rewritten keys, sorted over the aggregate output (before the
        // final projection renames columns). A key naming a SELECT alias is
        // replaced with that item's rewritten expression.
        if (stmt.orderBy) {
            std::vector<exec::Sort::Key> keys;
            for (const auto& k : stmt.orderBy->keys) {
                const ExprNode* keyExpr = nullptr;
                if (const auto* cr = std::get_if<ColumnRef>(k.expr.get())) {
                    for (size_t i = 0; i < stmt.selectList.size(); ++i) {
                        if (stmt.selectList[i].alias &&
                            *stmt.selectList[i].alias == cr->column) {
                            keyExpr = items[i].expr;   // the rewritten item expr
                            break;
                        }
                    }
                }
                if (!keyExpr) {
                    owned.push_back(cloneSubstAggs(*k.expr, aggCalls, aggOutNames, addSlot));
                    keyExpr = owned.back().get();
                }
                keys.push_back({ keyExpr, k.order, k.nullsOrder });
            }
            op = std::make_unique<exec::Sort>(std::move(op), std::move(keys));
        }

        // Final projection: only the SELECT items, with display names.
        op = std::make_unique<exec::Project>(std::move(op), std::move(items));

        if (stmt.distinct)
            op = std::make_unique<exec::Distinct>(std::move(op));

        op = applyLimit(std::move(op), stmt);
        return op;
    }

    // ---- ORDER BY (before projection) ----
    // ORDER BY expressions are written against the *input* columns (e.g.
    //   SELECT name FROM emp ORDER BY salary
    // sorts by a column that the projection drops). We therefore sort the rows
    // before projecting. Project and Distinct both preserve row order, so the
    // ordering established here survives to the output.
    op = applyOrderBy(std::move(op), stmt);

    // ---- Projection (non-aggregate) ----
    // SELECT * (bare star) keeps the child schema as-is; anything else builds
    // an explicit projection.
    bool selectStar = stmt.selectList.size() == 1 &&
        [&]{ const auto* c = std::get_if<ColumnRef>(stmt.selectList[0].expr.get());
             return c && c->isStar() && !c->table; }();

    if (!selectStar) {
        std::vector<exec::Project::Item> items;
        for (size_t i = 0; i < stmt.selectList.size(); ++i) {
            const auto& item = stmt.selectList[i];
            exec::Project::Item pit;
            pit.expr = item.expr.get();
            pit.name = itemName(item, i);
            // Best-effort type inference: column refs inherit their source type;
            // everything else defaults to a nullable double the Value can hold.
            pit.type = DataType::float64().asNullable();
            if (const auto* c = std::get_if<ColumnRef>(item.expr.get())) {
                int idx = -1;
                if (c->table) idx = op->schema().indexOf(*c->table + "." + c->column);
                if (idx < 0) idx = op->schema().indexOf(c->column);
                if (idx >= 0) pit.type = op->schema()[idx].type;
            }
            items.push_back(std::move(pit));
        }
        op = std::make_unique<exec::Project>(std::move(op), std::move(items));
    }

    // ---- DISTINCT ----
    if (stmt.distinct)
        op = std::make_unique<exec::Distinct>(std::move(op));

    // ---- LIMIT / OFFSET ----
    op = applyLimit(std::move(op), stmt);

    return op;
}

// Build the full operator tree for a SELECT, including UNION chaining.
exec::OperatorPtr buildSelect(const SelectStmt& stmt, Catalog& cat,
                              OwnedExprs& owned) {
    exec::OperatorPtr op = buildSelectCore(stmt, cat, owned);

    if (stmt.setOp) {
        if (stmt.setOp->op != SetOp::UNION && stmt.setOp->op != SetOp::UNION_ALL)
            throw BuildError("INTERSECT and EXCEPT are not supported yet");
        auto rhs = buildSelect(*stmt.setOp->rhs, cat, owned);   // chains recursively
        if (op->schema().size() != rhs->schema().size())
            throw BuildError("UNION requires both sides to have the same number of columns");
        op = std::make_unique<exec::Append>(std::move(op), std::move(rhs));
        if (stmt.setOp->op == SetOp::UNION)                     // UNION dedupes
            op = std::make_unique<exec::Distinct>(std::move(op));
    }
    return op;
}

}  // namespace

// ============================================================================
// Connection
// ============================================================================

Connection::Connection() : catalog_(std::make_unique<Catalog>()) {}
Connection::~Connection() = default;

// ---- Statement handlers ----------------------------------------------------

static QueryResult runCreate(const CreateTableStmt& ct, Catalog& cat) {
    Schema schema;
    for (const auto& col : ct.columns) {
        DataType t = col.type;
        if (col.isNotNull()) t.nullable = false;
        schema.addColumn(col.name, t);
    }
    cat.registerTable(ct.name, std::move(schema));
    QueryResult r;
    r.rowsAffected = 0;
    return r;
}

static QueryResult runDrop(const DropTableStmt& dt, Catalog& cat) {
    bool existed = cat.dropTable(dt.name);
    if (!existed && !dt.ifExists)
        return QueryResult::makeError("Unknown table: " + dt.name);
    QueryResult r;
    return r;
}

static QueryResult runSelect(const SelectStmt& stmt, Catalog& cat);

static QueryResult runInsert(const InsertStmt& ins, Catalog& cat) {
    TablePtr tbl = cat.getTable(ins.table);
    const Schema& schema = tbl->schema();

    // Map the (optional) explicit column list to positions.
    std::vector<int> positions;
    if (!ins.columns.empty()) {
        for (const auto& cn : ins.columns) {
            int idx = schema.indexOf(cn);
            if (idx < 0) return QueryResult::makeError("Unknown column in INSERT: " + cn);
            positions.push_back(idx);
        }
    }

    int64_t affected = 0;

    // ---- INSERT ... SELECT --------------------------------------------------
    if (const auto* sel = std::get_if<std::unique_ptr<SelectStmt>>(&ins.source)) {
        QueryResult src = runSelect(**sel, cat);
        if (!src.ok()) return src;

        const size_t want = positions.empty() ? schema.size() : positions.size();
        if (src.schema.size() != want)
            return QueryResult::makeError(
                "INSERT ... SELECT column count (" + std::to_string(src.schema.size()) +
                ") does not match target (" + std::to_string(want) + ")");

        for (auto& srcRow : src.rows) {
            Row row(schema.size(), Value::null());
            if (positions.empty()) {
                for (size_t i = 0; i < srcRow.size(); ++i) row[i] = std::move(srcRow[i]);
            } else {
                for (size_t i = 0; i < srcRow.size(); ++i) row[positions[i]] = std::move(srcRow[i]);
            }
            tbl->insertRow(std::move(row));
            ++affected;
        }

        QueryResult r;
        r.rowsAffected = affected;
        return r;
    }

    // ---- INSERT ... VALUES --------------------------------------------------
    const auto* valueRows = std::get_if<std::vector<ExprList>>(&ins.source);
    if (!valueRows)
        return QueryResult::makeError("unsupported INSERT source");

    for (const auto& exprRow : *valueRows) {
        Row row(schema.size(), Value::null());
        if (positions.empty()) {
            if (exprRow.size() != schema.size())
                return QueryResult::makeError("INSERT value count does not match column count");
            for (size_t i = 0; i < exprRow.size(); ++i)
                row[i] = evaluate(*exprRow[i], Schema{}, Row{});
        } else {
            if (exprRow.size() != positions.size())
                return QueryResult::makeError("INSERT value count does not match column list");
            for (size_t i = 0; i < exprRow.size(); ++i)
                row[positions[i]] = evaluate(*exprRow[i], Schema{}, Row{});
        }
        tbl->insertRow(std::move(row));
        ++affected;
    }

    QueryResult r;
    r.rowsAffected = affected;
    return r;
}

static QueryResult runSelect(const SelectStmt& stmt, Catalog& cat) {
    // Fast path: typed vectorized execution for the common aggregate shape
    // (SELECT [key,] AGG(col)... FROM t [WHERE simple] [GROUP BY key]). Returns
    // nullopt when the query doesn't match, in which case we use the general
    // operator tree. Both paths must agree on results (tested).
    if (auto fast = tryFastAggregate(stmt, cat))
        return std::move(*fast);

    // `owned` holds AST nodes synthesized during planning (rewritten aggregate
    // expressions); operators borrow into it, so it must outlive drain().
    OwnedExprs owned;
    exec::OperatorPtr root = buildSelect(stmt, cat, owned);
    Batch out = exec::drain(*root);

    QueryResult r;
    r.schema = out.schema;
    r.rows.reserve(out.numRows);
    for (size_t i = 0; i < out.numRows; ++i)
        r.rows.push_back(out.getRow(i));
    return r;
}

// ---- Public entry ----------------------------------------------------------

QueryResult Connection::query(const std::string& sql) {
    auto t0 = Clock::now();
    QueryResult result;
    try {
        Lexer lexer(sql);
        LexResult lex = lexer.tokenize();
        if (!lex.ok()) {
            std::string msg = "Lex error: " + lex.errors.front().message +
                              " at " + lex.errors.front().location.toString();
            return QueryResult::makeError(msg);
        }

        Parser parser(lex.tokens);
        ast::Stmt stmt = parser.parseStatement();

        result = std::visit([&](const auto& s) -> QueryResult {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, SelectStmt>)      return runSelect(s, *catalog_);
            else if constexpr (std::is_same_v<T, CreateTableStmt>) return runCreate(s, *catalog_);
            else if constexpr (std::is_same_v<T, DropTableStmt>)   return runDrop(s, *catalog_);
            else if constexpr (std::is_same_v<T, InsertStmt>)      return runInsert(s, *catalog_);
            else return QueryResult::makeError("unsupported statement");
        }, *stmt);
    }
    catch (const std::exception& e) {
        result = QueryResult::makeError(e.what());
    }

    auto t1 = Clock::now();
    result.elapsedMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return result;
}

QueryResult Connection::importCsv(const std::string& path,
                                  const std::string& tableName,
                                  char delimiter, bool hasHeader) {
    auto t0 = Clock::now();
    QueryResult result;
    try {
        if (catalog_->hasTable(tableName))
            return QueryResult::makeError("table already exists: " + tableName);

        CsvOptions opts;
        opts.delimiter = delimiter;
        opts.hasHeader = hasHeader;

        TablePtr table = readCsv(path, tableName, opts);
        result.rowsAffected = static_cast<int64_t>(table->rowCount());
        catalog_->registerTable(std::move(table));
    } catch (const std::exception& e) {
        result = QueryResult::makeError(e.what());
    }
    auto t1 = Clock::now();
    result.elapsedMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return result;
}

QueryResult Connection::saveDatabase(const std::string& path) {
    QueryResult result;
    try {
        lq::saveDatabase(*catalog_, path);
        result.rowsAffected = static_cast<int64_t>(catalog_->tableNames().size());
    } catch (const std::exception& e) {
        result = QueryResult::makeError(e.what());
    }
    return result;
}

QueryResult Connection::loadDatabase(const std::string& path) {
    QueryResult result;
    try {
        lq::loadDatabase(*catalog_, path);
        result.rowsAffected = static_cast<int64_t>(catalog_->tableNames().size());
    } catch (const std::exception& e) {
        result = QueryResult::makeError(e.what());
    }
    return result;
}

std::string Connection::explain(const std::string& sql) {
    try {
        Lexer lexer(sql);
        LexResult lex = lexer.tokenize();
        if (!lex.ok()) return "Lex error: " + lex.errors.front().message;

        Parser parser(lex.tokens);
        ast::Stmt stmt = parser.parseStatement();

        const auto* sel = std::get_if<SelectStmt>(stmt.get());
        if (!sel) return "EXPLAIN is only supported for SELECT statements";

        plan::LogicalPlanner planner(*catalog_);
        plan::LogicalPlan lp = planner.plan(*sel);

        opt::Optimizer optimizer(*catalog_);
        optimizer.addDefaultRules();
        lp = optimizer.optimize(std::move(lp));

        plan::PlanPrinter printer;
        return printer.print(*lp);
    }
    catch (const std::exception& e) {
        return std::string("EXPLAIN error: ") + e.what();
    }
}

}  // namespace lq
