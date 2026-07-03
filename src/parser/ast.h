#pragma once

// ============================================================================
// LiteQuery — ast.h
// Abstract Syntax Tree: all SQL statement and expression node types
//
// Design principles:
//   - Nodes are plain structs — no vtable, no inheritance hierarchy.
//   - std::variant<...all node types...> gives exhaustive dispatch via
//     std::visit without virtual dispatch overhead.
//   - Ownership is explicit: every child is held by unique_ptr so the tree
//     has clear single ownership and automatic deep destruction.
//   - SourceLocation is threaded through every node for error messages and
//     future EXPLAIN output that points back to the original SQL text.
//   - The Visitor pattern is implemented via a templated ASTVisitor base and
//     a free visit() function — callers get compile-time exhaustiveness checks
//     rather than silent no-ops from a forgotten virtual override.
//
// Node taxonomy
// ─────────────
//  Statements   SelectStmt, InsertStmt, CreateTableStmt, DropTableStmt
//  Table refs   TableRef, AliasedRef, JoinRef, SubqueryRef
//  Expressions  Literal, ColumnRef, UnaryExpr, BinaryExpr, FunctionCall,
//               CastExpr, CaseExpr, InExpr, BetweenExpr, IsNullExpr,
//               ExistsExpr, SubqueryExpr, WindowExpr
//  Clauses      SelectItem, FromClause, WhereClause, GroupByClause,
//               HavingClause, OrderByClause, LimitClause
// ============================================================================

#include "lexer.h"   // SourceLocation
#include "types.h"   // DataType, TypeId, Value

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lq {
namespace ast {

// ============================================================================
// Forward declarations — needed because nodes can recursively contain each other
// ============================================================================

// Expressions
struct Literal;
struct ColumnRef;
struct UnaryExpr;
struct BinaryExpr;
struct FunctionCall;
struct CastExpr;
struct CaseExpr;
struct InExpr;
struct BetweenExpr;
struct IsNullExpr;
struct ExistsExpr;
struct SubqueryExpr;
struct WindowExpr;

// Table references
struct TableRef;
struct AliasedRef;
struct JoinRef;
struct SubqueryRef;

// Statements
struct SelectStmt;
struct InsertStmt;
struct CreateTableStmt;
struct DropTableStmt;

// ============================================================================
// Expr — the variant that holds any expression node
//
// Stored as unique_ptr<ExprNode> everywhere to keep the variant's size
// manageable (a bare variant of all structs would be very large on the stack).
// ============================================================================

using ExprNode = std::variant<
    Literal,
    ColumnRef,
    UnaryExpr,
    BinaryExpr,
    FunctionCall,
    CastExpr,
    CaseExpr,
    InExpr,
    BetweenExpr,
    IsNullExpr,
    ExistsExpr,
    SubqueryExpr,
    WindowExpr
>;

using Expr    = std::unique_ptr<ExprNode>;
using ExprList = std::vector<Expr>;

// Helper: heap-allocate any expression node
// Construct T from args (aggregate or constructor), then move into the variant.
template <typename T, typename... Args>
Expr makeExpr(Args&&... args) {
    return std::make_unique<ExprNode>(T{std::forward<Args>(args)...});
}

// ============================================================================
// TableRefNode — the variant that holds any table-reference node
// ============================================================================

using TableRefNode = std::variant<
    TableRef,
    AliasedRef,
    JoinRef,
    SubqueryRef
>;

using TableRefPtr  = std::unique_ptr<TableRefNode>;

template <typename T, typename... Args>
TableRefPtr makeTableRef(Args&&... args) {
    return std::make_unique<TableRefNode>(T{std::forward<Args>(args)...});
}

// ============================================================================
// Statement — top-level variant
// ============================================================================

using StmtNode = std::variant<
    SelectStmt,
    InsertStmt,
    CreateTableStmt,
    DropTableStmt
>;

using Stmt = std::unique_ptr<StmtNode>;

template <typename T, typename... Args>
Stmt makeStmt(Args&&... args) {
    return std::make_unique<StmtNode>(T{std::forward<Args>(args)...});
}

// ============================================================================
// SortOrder / NullsOrder — shared enum used by ORDER BY and window frames
// ============================================================================

enum class SortOrder   : uint8_t { ASC, DESC };
enum class NullsOrder  : uint8_t { NULLS_FIRST, NULLS_LAST, UNSPECIFIED };
enum class JoinType    : uint8_t { INNER, LEFT, RIGHT, FULL, CROSS };
enum class SetOp       : uint8_t { UNION, UNION_ALL, INTERSECT, EXCEPT };
enum class FrameMode   : uint8_t { ROWS, RANGE };
enum class FrameBound  : uint8_t {
    UNBOUNDED_PRECEDING,
    CURRENT_ROW,
    UNBOUNDED_FOLLOWING,
    EXPR_PRECEDING,
    EXPR_FOLLOWING
};

// ============================================================================
//  E X P R E S S I O N   N O D E S
// ============================================================================

// ----------------------------------------------------------------------------
// Literal — a constant value baked into the query
// ----------------------------------------------------------------------------
struct Literal {
    Value          value;      // Wraps the actual typed constant (from types.h)
    SourceLocation location;

    // Convenience constructors
    static Literal integer(int64_t v, SourceLocation loc = {}) {
        return {Value(v), loc};
    }
    static Literal real(double v, SourceLocation loc = {}) {
        return {Value(v), loc};
    }
    static Literal string(std::string v, SourceLocation loc = {}) {
        return {Value(std::move(v)), loc};
    }
    static Literal boolean(bool v, SourceLocation loc = {}) {
        return {Value(v), loc};
    }
    static Literal null(SourceLocation loc = {}) {
        return {Value::null(), loc};
    }
};

// ----------------------------------------------------------------------------
// ColumnRef — a reference to a column, optionally qualified
//   Examples: col, table.col, schema.table.col
// ----------------------------------------------------------------------------
struct ColumnRef {
    std::optional<std::string> schema;   // Present in schema.table.col
    std::optional<std::string> table;    // Present in table.col
    std::string                column;   // Always present; "*" for SELECT *
    SourceLocation             location;

    bool isStar()      const noexcept { return column == "*"; }
    bool isQualified() const noexcept { return table.has_value(); }

    // Fully-qualified display string for error messages
    std::string toString() const {
        std::string s;
        if (schema) { s += *schema; s += '.'; }
        if (table)  { s += *table;  s += '.'; }
        s += column;
        return s;
    }
};

// ----------------------------------------------------------------------------
// UnaryExpr — prefix operators: NOT, -, +, IS NULL (see IsNullExpr)
// ----------------------------------------------------------------------------
enum class UnaryOp : uint8_t { NEGATE, PLUS, NOT };

struct UnaryExpr {
    UnaryOp        op;
    Expr           operand;
    SourceLocation location;
};

// ----------------------------------------------------------------------------
// BinaryExpr — arithmetic, comparison, and logical binary operators
// ----------------------------------------------------------------------------
enum class BinaryOp : uint8_t {
    // Arithmetic
    ADD, SUB, MUL, DIV, MOD, POW,
    // Comparison
    EQ, NEQ, LT, LTE, GT, GTE,
    // Logical
    AND, OR,
    // String
    CONCAT,   // ||
    LIKE, ILIKE,
    // JSON
    JSON_ARROW,    // ->
    JSON_ARROW2,   // ->>
};

struct BinaryExpr {
    BinaryOp       op;
    Expr           left;
    Expr           right;
    SourceLocation location;
};

// ----------------------------------------------------------------------------
// FunctionCall — scalar and aggregate function invocations
//   SUM(a), COUNT(*), COALESCE(a, b), DATE_TRUNC('month', ts)
// ----------------------------------------------------------------------------
struct FunctionCall {
    std::string    name;            // Always upper-cased by the parser
    ExprList       args;
    bool           distinct = false; // COUNT(DISTINCT col)
    bool           star     = false; // COUNT(*) — args will be empty
    SourceLocation location;

    bool isAggregate() const noexcept {
        return name == "SUM"   || name == "COUNT" || name == "AVG"  ||
               name == "MIN"   || name == "MAX"   || name == "STDDEV" ||
               name == "VARIANCE";
    }
};

// ----------------------------------------------------------------------------
// CastExpr — CAST(expr AS type) or the :: shorthand
// ----------------------------------------------------------------------------
struct CastExpr {
    Expr           expr;
    DataType       targetType;
    SourceLocation location;
};

// ----------------------------------------------------------------------------
// CaseExpr — CASE WHEN ... THEN ... ELSE ... END
//   Covers both simple CASE (with subject) and searched CASE (without).
// ----------------------------------------------------------------------------
struct CaseWhen {
    Expr condition;   // In simple CASE: right-hand side of implicit equality
    Expr result;
};

struct CaseExpr {
    std::optional<Expr>    subject;   // CASE <subject> WHEN ... — optional
    std::vector<CaseWhen>  whenClauses;
    std::optional<Expr>    elseExpr;
    SourceLocation         location;
};

// ----------------------------------------------------------------------------
// InExpr — expr [NOT] IN (val, val, ...) or expr [NOT] IN (subquery)
// ----------------------------------------------------------------------------
struct InExpr {
    Expr                       expr;
    std::variant<ExprList,
                 std::unique_ptr<SelectStmt>> values;  // list or subquery
    bool           negated = false;
    SourceLocation location;
};

// ----------------------------------------------------------------------------
// BetweenExpr — expr [NOT] BETWEEN low AND high
// ----------------------------------------------------------------------------
struct BetweenExpr {
    Expr           expr;
    Expr           low;
    Expr           high;
    bool           negated = false;
    SourceLocation location;
};

// ----------------------------------------------------------------------------
// IsNullExpr — expr IS [NOT] NULL
// ----------------------------------------------------------------------------
struct IsNullExpr {
    Expr           expr;
    bool           negated = false;  // IS NOT NULL
    SourceLocation location;
};

// ----------------------------------------------------------------------------
// ExistsExpr — [NOT] EXISTS (subquery)
// ----------------------------------------------------------------------------
struct ExistsExpr {
    std::unique_ptr<SelectStmt> subquery;
    bool           negated = false;
    SourceLocation location;
};

// ----------------------------------------------------------------------------
// SubqueryExpr — scalar subquery used as an expression: (SELECT MAX(x) FROM t)
// ----------------------------------------------------------------------------
struct SubqueryExpr {
    std::unique_ptr<SelectStmt> subquery;
    SourceLocation              location;
};

// ----------------------------------------------------------------------------
// WindowExpr — expr OVER (PARTITION BY ... ORDER BY ... ROWS BETWEEN ...)
// ----------------------------------------------------------------------------
struct FrameSpec {
    FrameMode  mode;
    FrameBound startBound;
    FrameBound endBound;
    // Expression offsets for EXPR_PRECEDING / EXPR_FOLLOWING
    std::optional<Expr> startOffset;
    std::optional<Expr> endOffset;
};

struct WindowExpr {
    Expr                     func;         // The aggregate/window function call
    ExprList                 partitionBy;
    std::vector<struct SortKey> orderBy;   // defined below
    std::optional<FrameSpec> frame;
    SourceLocation           location;
};

// ============================================================================
// SortKey — used in ORDER BY and window PARTITION ORDER BY
// ============================================================================
struct SortKey {
    Expr           expr;
    SortOrder      order      = SortOrder::ASC;
    NullsOrder     nullsOrder = NullsOrder::UNSPECIFIED;
    SourceLocation location;
};

// ============================================================================
// SelectItem — one item in the SELECT list
//   SELECT expr [[AS] alias], ...
// ============================================================================
struct SelectItem {
    Expr                       expr;
    std::optional<std::string> alias;
    SourceLocation             location;

    bool hasAlias() const noexcept { return alias.has_value(); }
};

// ============================================================================
//  T A B L E   R E F E R E N C E   N O D E S
// ============================================================================

// ----------------------------------------------------------------------------
// TableRef — a plain table or view name
//   FROM schema.table
// ----------------------------------------------------------------------------
struct TableRef {
    std::optional<std::string> schema;
    std::string                name;
    SourceLocation             location;
};

// ----------------------------------------------------------------------------
// AliasedRef — any table reference with an alias
//   FROM table AS t, (subquery) AS sq
// ----------------------------------------------------------------------------
struct AliasedRef {
    TableRefPtr    ref;
    std::string    alias;
    SourceLocation location;
};

// ----------------------------------------------------------------------------
// JoinRef — two table references joined together
//   t1 [INNER|LEFT|RIGHT|FULL|CROSS] JOIN t2 ON condition
// ----------------------------------------------------------------------------
struct JoinRef {
    JoinType    type;
    TableRefPtr left;
    TableRefPtr right;
    std::optional<Expr> condition;   // NULL for CROSS JOIN
    SourceLocation      location;
};

// ----------------------------------------------------------------------------
// SubqueryRef — an inline view: (SELECT ...) AS alias
// The alias lives in the wrapping AliasedRef, but SubqueryRef can appear
// directly in a JoinRef.left/right where the alias is optional.
// ----------------------------------------------------------------------------
struct SubqueryRef {
    std::unique_ptr<SelectStmt> subquery;
    SourceLocation              location;
};

// ============================================================================
// Clause structs — components of a SELECT statement
// ============================================================================

struct FromClause {
    std::vector<TableRefPtr> tables;   // Comma-separated table refs
    SourceLocation           location;
};

struct WhereClause {
    Expr           predicate;
    SourceLocation location;
};

struct GroupByClause {
    ExprList       keys;
    SourceLocation location;
};

struct HavingClause {
    Expr           predicate;
    SourceLocation location;
};

struct OrderByClause {
    std::vector<SortKey> keys;
    SourceLocation       location;
};

struct LimitClause {
    std::optional<Expr> limit;
    std::optional<Expr> offset;
    SourceLocation      location;
};

// ============================================================================
//  S T A T E M E N T   N O D E S
// ============================================================================

// ----------------------------------------------------------------------------
// SelectStmt — the full SELECT statement
// ----------------------------------------------------------------------------
struct SelectStmt {
    bool                        distinct = false;
    std::vector<SelectItem>     selectList;    // Empty means SELECT *
    std::optional<FromClause>   from;
    std::optional<WhereClause>  where;
    std::optional<GroupByClause>groupBy;
    std::optional<HavingClause> having;
    std::optional<OrderByClause>orderBy;
    std::optional<LimitClause>  limit;

    // UNION / INTERSECT / EXCEPT chaining
    struct SetOpTail {
        SetOp                       op;
        std::unique_ptr<SelectStmt> rhs;
    };
    std::optional<SetOpTail> setOp;

    SourceLocation location;

    bool hasAggregates() const noexcept {
        return groupBy.has_value() || having.has_value();
    }
};

// ----------------------------------------------------------------------------
// InsertStmt — INSERT INTO table [(cols)] VALUES (...) | SELECT ...
// ----------------------------------------------------------------------------
struct InsertStmt {
    std::string              table;
    std::optional<std::string> schema;
    std::vector<std::string> columns;    // Empty = insert all columns in order

    // Either a VALUES list or a SELECT subquery as the source
    std::variant<
        std::vector<ExprList>,           // VALUES (row1), (row2), ...
        std::unique_ptr<SelectStmt>      // INSERT INTO t SELECT ...
    > source;

    SourceLocation location;
};

// ----------------------------------------------------------------------------
// ColumnConstraint — constraints on a column in CREATE TABLE
// ----------------------------------------------------------------------------
struct ColumnConstraint {
    enum class Kind : uint8_t {
        NOT_NULL,
        PRIMARY_KEY,
        UNIQUE,
        DEFAULT,
        CHECK,
    };
    Kind                kind;
    std::optional<Expr> expr;   // For DEFAULT and CHECK constraints
    SourceLocation      location;
};

// ----------------------------------------------------------------------------
// ColumnSpec — one column definition in CREATE TABLE
// ----------------------------------------------------------------------------
struct ColumnSpec {
    std::string                    name;
    DataType                       type;
    std::vector<ColumnConstraint>  constraints;
    SourceLocation                 location;

    bool isNotNull()    const noexcept {
        for (auto& c : constraints)
            if (c.kind == ColumnConstraint::Kind::NOT_NULL) return true;
        return false;
    }
    bool isPrimaryKey() const noexcept {
        for (auto& c : constraints)
            if (c.kind == ColumnConstraint::Kind::PRIMARY_KEY) return true;
        return false;
    }
};

// ----------------------------------------------------------------------------
// CreateTableStmt
// ----------------------------------------------------------------------------
struct CreateTableStmt {
    std::optional<std::string> schema;
    std::string                name;
    std::vector<ColumnSpec>    columns;
    bool                       ifNotExists = false;
    SourceLocation             location;
};

// ----------------------------------------------------------------------------
// DropTableStmt
// ----------------------------------------------------------------------------
struct DropTableStmt {
    std::optional<std::string> schema;
    std::string                name;
    bool                       ifExists = false;
    SourceLocation             location;
};

// ============================================================================
//  V I S I T O R   I N F R A S T R U C T U R E
// ============================================================================
//
// ASTVisitor<Derived, ReturnType> — CRTP base for tree walks.
//
// Derived class only needs to override the visit() overloads it cares about.
// The default for each node type calls visitDefault(), which is a no-op by
// default. This gives a safe fallback while still letting the compiler warn
// about unhandled cases if the derived class marks visitDefault deleted.
//
// Usage:
//   struct TypeChecker : ASTVisitor<TypeChecker, DataType> {
//       DataType visit(const BinaryExpr& e) { ... }
//       DataType visit(const Literal& e)    { ... }
//       // All other nodes fall through to visitDefault → DataType{}
//   };
//
//   TypeChecker tc;
//   DataType dt = visitExpr(tc, *some_expr_node);
// ============================================================================

template <typename Derived, typename Ret = void>
struct ASTVisitor {
    // Default handler — override in Derived to catch-all
    template<typename _N> Ret visitDefault(const _N&) {
        if constexpr (std::is_void_v<Ret>) return;
        else return Ret{};
    }

    // Expression nodes
    Ret visit(const Literal&      n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const ColumnRef&    n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const UnaryExpr&    n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const BinaryExpr&   n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const FunctionCall& n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const CastExpr&     n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const CaseExpr&     n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const InExpr&       n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const BetweenExpr&  n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const IsNullExpr&   n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const ExistsExpr&   n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const SubqueryExpr& n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const WindowExpr&   n) { return static_cast<Derived*>(this)->visitDefault(n); }

    // Statement nodes
    Ret visit(const SelectStmt&      n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const InsertStmt&      n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const CreateTableStmt& n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const DropTableStmt&   n) { return static_cast<Derived*>(this)->visitDefault(n); }

    // Table reference nodes
    Ret visit(const TableRef&    n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const AliasedRef&  n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const JoinRef&     n) { return static_cast<Derived*>(this)->visitDefault(n); }
    Ret visit(const SubqueryRef& n) { return static_cast<Derived*>(this)->visitDefault(n); }
};

// ---- Free dispatch functions ------------------------------------------------

template <typename Visitor>
auto visitExpr(Visitor& v, const ExprNode& node) {
    return std::visit([&v](const auto& n) { return v.visit(n); }, node);
}

template <typename Visitor>
auto visitStmt(Visitor& v, const StmtNode& node) {
    return std::visit([&v](const auto& n) { return v.visit(n); }, node);
}

template <typename Visitor>
auto visitTableRef(Visitor& v, const TableRefNode& node) {
    return std::visit([&v](const auto& n) { return v.visit(n); }, node);
}

// ============================================================================
// ASTDebugPrinter — walk the tree and emit indented text
// Usage:
//   ASTDebugPrinter p;
//   p.printStmt(*stmt);
// ============================================================================

class ASTDebugPrinter : public ASTVisitor<ASTDebugPrinter, std::string> {
public:
    using ASTVisitor<ASTDebugPrinter, std::string>::visit;
public:
    explicit ASTDebugPrinter(int indent = 0) : indent_(indent) {}

    std::string visit(const Literal& n) {
        return ind() + "Literal(" + n.value.toString() + ")";
    }
    std::string visit(const ColumnRef& n) {
        return ind() + "ColumnRef(" + n.toString() + ")";
    }
    std::string visit(const UnaryExpr& n) {
        std::string opStr;
        switch (n.op) {
            case UnaryOp::NEGATE: opStr = "-"; break;
            case UnaryOp::PLUS:   opStr = "+"; break;
            case UnaryOp::NOT:    opStr = "NOT"; break;
        }
        return ind() + "UnaryExpr(" + opStr + ")\n" + child(*n.operand);
    }
    std::string visit(const BinaryExpr& n) {
        return ind() + "BinaryExpr(" + binaryOpStr(n.op) + ")\n"
             + child(*n.left) + "\n" + child(*n.right);
    }
    std::string visit(const FunctionCall& n) {
        std::string s = ind() + "FunctionCall(" + n.name;
        if (n.distinct) s += " DISTINCT";
        if (n.star)     s += " *";
        s += ")";
        for (const auto& a : n.args) s += "\n" + child(*a);
        return s;
    }
    std::string visit(const CastExpr& n) {
        return ind() + "Cast(AS " + n.targetType.toString() + ")\n" + child(*n.expr);
    }
    std::string visit(const IsNullExpr& n) {
        return ind() + std::string(n.negated ? "IS NOT NULL\n" : "IS NULL\n") + child(*n.expr);
    }
    std::string visit(const BetweenExpr& n) {
        return ind() + std::string(n.negated ? "NOT BETWEEN\n" : "BETWEEN\n")
             + child(*n.expr) + "\n" + child(*n.low) + "\n" + child(*n.high);
    }
    std::string visit(const SelectStmt& n) {
        std::string s = ind() + "SelectStmt";
        if (n.distinct) s += " DISTINCT";
        s += "\n" + ind() + "  SelectList:";
        for (const auto& item : n.selectList) {
            ++indent_;
            s += "\n" + visitExpr(*this, *item.expr);
            if (item.alias) s += " AS " + *item.alias;
            --indent_;
        }
        if (n.from) {
            s += "\n" + ind() + "  From:";
            for (const auto& t : n.from->tables) {
                ++indent_; ++indent_;
                s += "\n" + visitTableRef(*this, *t);
                --indent_; --indent_;
            }
        }
        if (n.where) {
            s += "\n" + ind() + "  Where:\n" + child(*n.where->predicate);
        }
        if (n.groupBy) {
            s += "\n" + ind() + "  GroupBy:";
            for (const auto& k : n.groupBy->keys) {
                ++indent_;
                s += "\n" + visitExpr(*this, *k);
                --indent_;
            }
        }
        if (n.orderBy) {
            s += "\n" + ind() + "  OrderBy:";
            for (const auto& k : n.orderBy->keys) {
                ++indent_;
                s += "\n" + visitExpr(*this, *k.expr);
                s += (k.order == SortOrder::ASC ? " ASC" : " DESC");
                --indent_;
            }
        }
        if (n.limit) {
            if (n.limit->limit)  s += "\n" + ind() + "  Limit:"  + child(**n.limit->limit);
            if (n.limit->offset) s += "\n" + ind() + "  Offset:" + child(**n.limit->offset);
        }
        return s;
    }
    std::string visit(const TableRef& n) {
        std::string s = ind() + "TableRef(";
        if (n.schema) s += *n.schema + ".";
        return s + n.name + ")";
    }
    std::string visit(const AliasedRef& n) {
        return ind() + "AliasedRef(AS " + n.alias + ")\n"
             + ind() + "  " + visitTableRef(*this, *n.ref);
    }
    std::string visit(const JoinRef& n) {
        std::string s = ind() + "JoinRef(" + joinTypeStr(n.type) + ")\n";
        ++indent_;
        s += visitTableRef(*this, *n.left) + "\n";
        s += visitTableRef(*this, *n.right);
        if (n.condition) s += "\n" + child(**n.condition);
        --indent_;
        return s;
    }
    std::string visit(const CreateTableStmt& n) {
        std::string s = ind() + "CreateTable(" + n.name + ")";
        for (const auto& col : n.columns)
            s += "\n" + ind() + "  col:" + col.name + " " + col.type.toString();
        return s;
    }
    std::string visit(const DropTableStmt& n) {
        return ind() + "DropTable(" + n.name + std::string(n.ifExists ? " IF EXISTS" : "") + ")";
    }

private:
    int indent_;

    std::string ind() const { return std::string(indent_ * 2, ' '); }

    std::string child(const ExprNode& n) {
        ++indent_;
        auto s = visitExpr(*this, n);
        --indent_;
        return s;
    }

    static std::string binaryOpStr(BinaryOp op) {
        switch (op) {
            case BinaryOp::ADD:     return "+";
            case BinaryOp::SUB:     return "-";
            case BinaryOp::MUL:     return "*";
            case BinaryOp::DIV:     return "/";
            case BinaryOp::MOD:     return "%";
            case BinaryOp::POW:     return "^";
            case BinaryOp::EQ:      return "=";
            case BinaryOp::NEQ:     return "<>";
            case BinaryOp::LT:      return "<";
            case BinaryOp::LTE:     return "<=";
            case BinaryOp::GT:      return ">";
            case BinaryOp::GTE:     return ">=";
            case BinaryOp::AND:     return "AND";
            case BinaryOp::OR:      return "OR";
            case BinaryOp::CONCAT:  return "||";
            case BinaryOp::LIKE:    return "LIKE";
            case BinaryOp::ILIKE:   return "ILIKE";
            case BinaryOp::JSON_ARROW:  return "->";
            case BinaryOp::JSON_ARROW2: return "->>";
        }
        return "?";
    }

    static std::string joinTypeStr(JoinType jt) {
        switch (jt) {
            case JoinType::INNER: return "INNER";
            case JoinType::LEFT:  return "LEFT";
            case JoinType::RIGHT: return "RIGHT";
            case JoinType::FULL:  return "FULL";
            case JoinType::CROSS: return "CROSS";
        }
        return "?";
    }
};

}  // namespace ast
}  // namespace lq
