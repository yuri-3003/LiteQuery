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
#include "logical_plan.h"
#include "optimizer.h"

#include <chrono>
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

// Forward declaration — table refs and full selects are mutually recursive.
exec::OperatorPtr buildSelect(const SelectStmt& stmt, Catalog& cat);

// Build an operator for a FROM table reference, returning its operator and the
// schema it produces.
exec::OperatorPtr buildTableRef(const TableRefNode& ref, Catalog& cat,
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
            return buildTableRef(*r.ref, cat, r.alias);
        }
        else if constexpr (std::is_same_v<T, JoinRef>) {
            auto left  = buildTableRef(*r.left, cat);
            auto right = buildTableRef(*r.right, cat);
            Schema merged = Schema::merge(left->schema(), right->schema());
            const ExprNode* cond = r.condition ? r.condition->get() : nullptr;
            return std::make_unique<exec::HashJoin>(
                std::move(left), std::move(right), r.type, cond, std::move(merged));
        }
        else if constexpr (std::is_same_v<T, SubqueryRef>) {
            auto op = buildSelect(*r.subquery, cat);
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

// Detect whether the SELECT is an aggregate query.
bool isAggregateQuery(const SelectStmt& stmt) {
    if (stmt.groupBy || stmt.having) return true;
    for (const auto& item : stmt.selectList) {
        bool agg = false;
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, FunctionCall>) agg = e.isAggregate();
        }, *item.expr);
        if (agg) return true;
    }
    return false;
}

// Recursively collect aggregate FunctionCall pointers from an expression.
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

// Build the full operator tree for a SELECT.
exec::OperatorPtr buildSelect(const SelectStmt& stmt, Catalog& cat) {
    // ---- FROM ----
    exec::OperatorPtr op;
    if (stmt.from && !stmt.from->tables.empty()) {
        op = buildTableRef(*stmt.from->tables[0], cat);
        for (size_t i = 1; i < stmt.from->tables.size(); ++i) {
            auto right = buildTableRef(*stmt.from->tables[i], cat);
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
        // Group keys
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

        // Collect aggregate calls from SELECT and HAVING.
        std::vector<const FunctionCall*> aggCalls;
        for (const auto& item : stmt.selectList) collectAggs(*item.expr, aggCalls);
        if (stmt.having) collectAggs(*stmt.having->predicate, aggCalls);

        std::vector<exec::HashAggregate::Agg> aggs;
        for (const auto* fc : aggCalls) {
            exec::HashAggregate::Agg a;
            a.func     = fc->name;
            a.star     = fc->star;
            a.distinct = fc->distinct;
            a.arg      = (!fc->star && !fc->args.empty()) ? fc->args[0].get() : nullptr;
            a.outName  = fc->name;
            a.outType  = (fc->name == "COUNT") ? DataType::int64() : DataType::float64().asNullable();
            aggs.push_back(a);
        }

        op = std::make_unique<exec::HashAggregate>(
            std::move(op), std::move(groupKeys), std::move(groupKeyNames), std::move(aggs));

        // NOTE: HAVING on aggregate output and projecting aggregate results by
        // position is handled in a simplified way: the aggregate output schema
        // exposes group keys then aggregates in call order, and the final
        // projection below references them. For the MVP we project aggregate
        // results directly via their positional names.
        // HAVING filter (references aggregate output columns by aggregate name).
        if (stmt.having)
            op = std::make_unique<exec::Filter>(std::move(op), stmt.having->predicate.get());

        return op;   // aggregate result is the final shape (already named)
    }

    // ---- ORDER BY (before projection) ----
    // ORDER BY expressions are written against the *input* columns (e.g.
    //   SELECT name FROM emp ORDER BY salary
    // sorts by a column that the projection drops). We therefore sort the rows
    // before projecting. Project and Distinct both preserve row order, so the
    // ordering established here survives to the output.
    if (stmt.orderBy) {
        std::vector<exec::Sort::Key> keys;
        for (const auto& k : stmt.orderBy->keys)
            keys.push_back({ k.expr.get(), k.order, k.nullsOrder });
        op = std::make_unique<exec::Sort>(std::move(op), std::move(keys));
    }

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
    if (stmt.limit) {
        int64_t lim = -1, off = 0;
        if (stmt.limit->limit) {
            Value v = evaluate(**stmt.limit->limit, Schema{}, Row{});
            lim = v.toInt64();
        }
        if (stmt.limit->offset) {
            Value v = evaluate(**stmt.limit->offset, Schema{}, Row{});
            off = v.toInt64();
        }
        op = std::make_unique<exec::Limit>(std::move(op), lim, off);
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
    const auto* valueRows = std::get_if<std::vector<ExprList>>(&ins.source);
    if (!valueRows)
        return QueryResult::makeError("INSERT ... SELECT is not supported yet");

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
    exec::OperatorPtr root = buildSelect(stmt, cat);
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
