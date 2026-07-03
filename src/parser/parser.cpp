// ============================================================================
// LiteQuery — parser.cpp
// Recursive-descent statement grammar + Pratt expression parser.
// ============================================================================

#include "parser.h"

#include <algorithm>
#include <cassert>

namespace lq {

using namespace ast;

// ============================================================================
// Construction & token cursor
// ============================================================================

Parser::Parser(const std::vector<Token>& tokens) noexcept : tokens_(tokens) {
    assert(!tokens_.empty() && "token stream must end with END_OF_FILE");
}

const Token& Parser::peek(int offset) const noexcept {
    size_t idx = pos_ + static_cast<size_t>(offset);
    if (idx >= tokens_.size()) return tokens_.back();  // EOF is sticky
    return tokens_[idx];
}

const Token& Parser::current() const noexcept { return peek(0); }

const Token& Parser::advance() noexcept {
    const Token& t = current();
    if (!isAtEnd()) ++pos_;
    return t;
}

bool Parser::check(TokenKind k) const noexcept { return current().kind == k; }

bool Parser::match(TokenKind k) noexcept {
    if (check(k)) { advance(); return true; }
    return false;
}

const Token& Parser::expect(TokenKind k, const char* what) {
    if (!check(k)) {
        error(std::string("expected ") + what + " but found '" +
              std::string(tokenKindName(current().kind)) + "'");
    }
    return advance();
}

bool Parser::isAtEnd() const noexcept {
    return current().kind == TokenKind::END_OF_FILE;
}

void Parser::error(const std::string& msg) const {
    throw ParseError(msg, current().location);
}

// ============================================================================
// Statement dispatch
// ============================================================================

ast::Stmt Parser::parseStatement() {
    ast::Stmt stmt;
    switch (current().kind) {
        case TokenKind::KW_SELECT:
            stmt = makeStmt<SelectStmt>();
            *std::get_if<SelectStmt>(stmt.get()) = std::move(*parseSelect());
            break;
        case TokenKind::KW_INSERT: stmt = parseInsert();      break;
        case TokenKind::KW_CREATE: stmt = parseCreateTable(); break;
        case TokenKind::KW_DROP:   stmt = parseDropTable();   break;
        default:
            error("expected a statement (SELECT/INSERT/CREATE/DROP)");
    }
    match(TokenKind::SEMICOLON);              // optional trailing ';'
    if (!isAtEnd())
        error("unexpected trailing tokens after statement");
    return stmt;
}

// ============================================================================
// SELECT
// ============================================================================

std::unique_ptr<SelectStmt> Parser::parseSelect() {
    auto stmt = std::make_unique<SelectStmt>();
    stmt->location = current().location;
    expect(TokenKind::KW_SELECT, "SELECT");

    if (match(TokenKind::KW_DISTINCT)) stmt->distinct = true;
    else                               match(TokenKind::KW_ALL);   // SELECT ALL

    stmt->selectList = parseSelectList();

    if (check(TokenKind::KW_FROM))
        stmt->from = parseFrom();

    if (match(TokenKind::KW_WHERE)) {
        WhereClause w;
        w.location  = current().location;
        w.predicate = parseExpression();
        stmt->where = std::move(w);
    }

    if (match(TokenKind::KW_GROUP)) {
        expect(TokenKind::KW_BY, "BY after GROUP");
        GroupByClause g;
        g.location = current().location;
        do {
            g.keys.push_back(parseExpression());
        } while (match(TokenKind::COMMA));
        stmt->groupBy = std::move(g);
    }

    if (match(TokenKind::KW_HAVING)) {
        HavingClause h;
        h.location  = current().location;
        h.predicate = parseExpression();
        stmt->having = std::move(h);
    }

    if (match(TokenKind::KW_ORDER)) {
        expect(TokenKind::KW_BY, "BY after ORDER");
        OrderByClause ob;
        ob.location = current().location;
        ob.keys     = parseOrderBy();
        stmt->orderBy = std::move(ob);
    }

    if (match(TokenKind::KW_LIMIT)) {
        LimitClause lim;
        lim.location = current().location;
        lim.limit    = parseExpression();
        if (match(TokenKind::KW_OFFSET))
            lim.offset = parseExpression();
        stmt->limit = std::move(lim);
    }

    // Set operations: UNION [ALL] <select>
    if (check(TokenKind::KW_UNION)) {
        advance();
        SelectStmt::SetOpTail tail;
        tail.op  = match(TokenKind::KW_ALL) ? SetOp::UNION_ALL : SetOp::UNION;
        tail.rhs = parseSelect();
        stmt->setOp = std::move(tail);
    }

    return stmt;
}

std::vector<SelectItem> Parser::parseSelectList() {
    std::vector<SelectItem> items;
    do {
        SelectItem item;
        item.location = current().location;

        // A bare '*' is a star column ref.
        if (check(TokenKind::OP_STAR)) {
            advance();
            ColumnRef star;
            star.column = "*";
            item.expr   = makeExpr<ColumnRef>(std::move(star));
        } else {
            item.expr = parseExpression();
            // Optional alias: [AS] identifier
            if (match(TokenKind::KW_AS)) {
                item.alias = std::string(expect(TokenKind::IDENT, "alias after AS").lexeme);
            } else if (check(TokenKind::IDENT)) {
                item.alias = std::string(advance().lexeme);
            }
        }
        items.push_back(std::move(item));
    } while (match(TokenKind::COMMA));
    return items;
}

// ============================================================================
// FROM / table references / joins
// ============================================================================

ast::FromClause Parser::parseFrom() {
    FromClause from;
    from.location = current().location;
    expect(TokenKind::KW_FROM, "FROM");
    do {
        from.tables.push_back(parseTableRef());
    } while (match(TokenKind::COMMA));
    return from;
}

// A full table reference: a primary ref possibly followed by a chain of JOINs.
ast::TableRefPtr Parser::parseTableRef() {
    return parseJoinChain(parsePrimaryTableRef());
}

ast::TableRefPtr Parser::parseJoinChain(ast::TableRefPtr left) {
    for (;;) {
        JoinType jt;
        SourceLocation loc = current().location;

        if (match(TokenKind::KW_CROSS)) {
            expect(TokenKind::KW_JOIN, "JOIN after CROSS");
            jt = JoinType::CROSS;
        } else if (match(TokenKind::KW_INNER)) {
            expect(TokenKind::KW_JOIN, "JOIN after INNER");
            jt = JoinType::INNER;
        } else if (match(TokenKind::KW_LEFT)) {
            match(TokenKind::KW_OUTER);
            expect(TokenKind::KW_JOIN, "JOIN after LEFT");
            jt = JoinType::LEFT;
        } else if (match(TokenKind::KW_RIGHT)) {
            match(TokenKind::KW_OUTER);
            expect(TokenKind::KW_JOIN, "JOIN after RIGHT");
            jt = JoinType::RIGHT;
        } else if (match(TokenKind::KW_FULL)) {
            match(TokenKind::KW_OUTER);
            expect(TokenKind::KW_JOIN, "JOIN after FULL");
            jt = JoinType::FULL;
        } else if (match(TokenKind::KW_JOIN)) {
            jt = JoinType::INNER;                 // bare JOIN == INNER JOIN
        } else {
            break;                                // no more joins
        }

        JoinRef join;
        join.type     = jt;
        join.location = loc;
        join.left     = std::move(left);
        join.right    = parsePrimaryTableRef();
        if (jt != JoinType::CROSS) {
            expect(TokenKind::KW_ON, "ON after JOIN");
            join.condition = parseExpression();
        }
        left = makeTableRef<JoinRef>(std::move(join));
    }
    return left;
}

ast::TableRefPtr Parser::parsePrimaryTableRef() {
    SourceLocation loc = current().location;
    ast::TableRefPtr ref;

    if (match(TokenKind::LPAREN)) {
        // Sub-query: ( SELECT ... )
        SubqueryRef sub;
        sub.location = loc;
        sub.subquery = parseSelect();
        expect(TokenKind::RPAREN, ") to close subquery");
        ref = makeTableRef<SubqueryRef>(std::move(sub));
    } else {
        // schema.table or table
        std::string first = std::string(expect(TokenKind::IDENT, "table name").lexeme);
        TableRef tr;
        tr.location = loc;
        if (match(TokenKind::DOT)) {
            tr.schema = first;
            tr.name   = std::string(expect(TokenKind::IDENT, "table name after '.'").lexeme);
        } else {
            tr.name = first;
        }
        ref = makeTableRef<TableRef>(std::move(tr));
    }

    // Optional alias: [AS] identifier (but not a JOIN keyword or clause keyword)
    std::string alias;
    if (match(TokenKind::KW_AS)) {
        alias = std::string(expect(TokenKind::IDENT, "alias after AS").lexeme);
    } else if (check(TokenKind::IDENT)) {
        alias = std::string(advance().lexeme);
    }
    if (!alias.empty()) {
        AliasedRef ar;
        ar.location = loc;
        ar.ref      = std::move(ref);
        ar.alias    = std::move(alias);
        ref = makeTableRef<AliasedRef>(std::move(ar));
    }
    return ref;
}

std::vector<SortKey> Parser::parseOrderBy() {
    std::vector<SortKey> keys;
    do {
        SortKey key;
        key.location = current().location;
        key.expr     = parseExpression();
        if      (match(TokenKind::KW_ASC))  key.order = SortOrder::ASC;
        else if (match(TokenKind::KW_DESC)) key.order = SortOrder::DESC;
        if (match(TokenKind::KW_NULLS)) {
            if      (match(TokenKind::KW_FIRST)) key.nullsOrder = NullsOrder::NULLS_FIRST;
            else if (match(TokenKind::KW_LAST))  key.nullsOrder = NullsOrder::NULLS_LAST;
            else error("expected FIRST or LAST after NULLS");
        }
        keys.push_back(std::move(key));
    } while (match(TokenKind::COMMA));
    return keys;
}

// ============================================================================
// Expressions — Pratt parser
//
// Precedence (low → high):
//   OR < AND < NOT < comparison/IS/IN/LIKE/BETWEEN < +,- < *,/,% < ^ < unary
// ============================================================================

namespace {

// Binding power of a binary operator token, or -1 if it is not a binary op.
int binaryPrecedence(TokenKind k) {
    switch (k) {
        case TokenKind::KW_OR:       return 1;
        case TokenKind::KW_AND:      return 2;
        // comparisons at level 4 (NOT is level 3, handled in unary)
        case TokenKind::OP_EQ:
        case TokenKind::OP_NEQ:
        case TokenKind::OP_LT:
        case TokenKind::OP_LTE:
        case TokenKind::OP_GT:
        case TokenKind::OP_GTE:
        case TokenKind::KW_LIKE:
        case TokenKind::KW_ILIKE:    return 4;
        case TokenKind::OP_CONCAT:   return 5;
        case TokenKind::OP_PLUS:
        case TokenKind::OP_MINUS:    return 6;
        case TokenKind::OP_STAR:
        case TokenKind::OP_SLASH:
        case TokenKind::OP_PERCENT:  return 7;
        case TokenKind::OP_CARET:    return 8;   // right-associative
        default:                     return -1;
    }
}

bool isRightAssoc(TokenKind k) { return k == TokenKind::OP_CARET; }

BinaryOp toBinaryOp(TokenKind k) {
    switch (k) {
        case TokenKind::KW_OR:      return BinaryOp::OR;
        case TokenKind::KW_AND:     return BinaryOp::AND;
        case TokenKind::OP_EQ:      return BinaryOp::EQ;
        case TokenKind::OP_NEQ:     return BinaryOp::NEQ;
        case TokenKind::OP_LT:      return BinaryOp::LT;
        case TokenKind::OP_LTE:     return BinaryOp::LTE;
        case TokenKind::OP_GT:      return BinaryOp::GT;
        case TokenKind::OP_GTE:     return BinaryOp::GTE;
        case TokenKind::KW_LIKE:    return BinaryOp::LIKE;
        case TokenKind::KW_ILIKE:   return BinaryOp::ILIKE;
        case TokenKind::OP_CONCAT:  return BinaryOp::CONCAT;
        case TokenKind::OP_PLUS:    return BinaryOp::ADD;
        case TokenKind::OP_MINUS:   return BinaryOp::SUB;
        case TokenKind::OP_STAR:    return BinaryOp::MUL;
        case TokenKind::OP_SLASH:   return BinaryOp::DIV;
        case TokenKind::OP_PERCENT: return BinaryOp::MOD;
        case TokenKind::OP_CARET:   return BinaryOp::POW;
        default: return BinaryOp::ADD;  // unreachable
    }
}

}  // namespace

ast::Expr Parser::parseExpression() { return parseExpr(0); }

ast::Expr Parser::parseExpr(int minPrecedence) {
    ast::Expr left = parseUnary();
    left = parsePostfix(std::move(left));   // IS NULL / BETWEEN / IN after a primary

    for (;;) {
        TokenKind k = current().kind;
        int prec = binaryPrecedence(k);
        if (prec < minPrecedence || prec < 0) break;

        SourceLocation loc = current().location;
        advance();

        int nextMin = isRightAssoc(k) ? prec : prec + 1;
        ast::Expr right = parseExpr(nextMin);

        BinaryExpr be;
        be.op       = toBinaryOp(k);
        be.left     = std::move(left);
        be.right    = std::move(right);
        be.location = loc;
        left = makeExpr<BinaryExpr>(std::move(be));

        // Allow chained postfix (e.g. a + b BETWEEN ...) — re-check postfix.
        left = parsePostfix(std::move(left));
    }
    return left;
}

ast::Expr Parser::parseUnary() {
    SourceLocation loc = current().location;
    if (match(TokenKind::KW_NOT)) {
        UnaryExpr u; u.op = UnaryOp::NOT; u.location = loc;
        u.operand = parseExpr(3);          // NOT binds tighter than AND/OR
        return makeExpr<UnaryExpr>(std::move(u));
    }
    if (match(TokenKind::OP_MINUS)) {
        UnaryExpr u; u.op = UnaryOp::NEGATE; u.location = loc;
        u.operand = parseUnary();
        return makeExpr<UnaryExpr>(std::move(u));
    }
    if (match(TokenKind::OP_PLUS)) {
        UnaryExpr u; u.op = UnaryOp::PLUS; u.location = loc;
        u.operand = parseUnary();
        return makeExpr<UnaryExpr>(std::move(u));
    }
    return parsePrimary();
}

// Postfix operators that apply to an already-parsed expression.
ast::Expr Parser::parsePostfix(ast::Expr left) {
    SourceLocation loc = current().location;

    // IS [NOT] NULL
    if (check(TokenKind::KW_IS)) {
        advance();
        bool negated = match(TokenKind::KW_NOT);
        expect(TokenKind::LIT_NULL, "NULL after IS [NOT]");
        IsNullExpr e; e.expr = std::move(left); e.negated = negated; e.location = loc;
        return parsePostfix(makeExpr<IsNullExpr>(std::move(e)));
    }

    // [NOT] BETWEEN low AND high    and    [NOT] IN (...)  and  [NOT] LIKE
    bool negated = false;
    if (check(TokenKind::KW_NOT) &&
        (peek(1).kind == TokenKind::KW_BETWEEN || peek(1).kind == TokenKind::KW_IN ||
         peek(1).kind == TokenKind::KW_LIKE   || peek(1).kind == TokenKind::KW_ILIKE)) {
        advance();
        negated = true;
    }

    if (match(TokenKind::KW_BETWEEN)) {
        BetweenExpr e;
        e.expr    = std::move(left);
        e.negated = negated;
        e.location = loc;
        e.low  = parseExpr(4 + 1);          // bind tighter than AND to stop at AND
        expect(TokenKind::KW_AND, "AND in BETWEEN");
        e.high = parseExpr(4 + 1);
        return parsePostfix(makeExpr<BetweenExpr>(std::move(e)));
    }

    if (match(TokenKind::KW_IN)) {
        InExpr e;
        e.expr    = std::move(left);
        e.negated = negated;
        e.location = loc;
        expect(TokenKind::LPAREN, "( after IN");
        if (check(TokenKind::KW_SELECT)) {
            e.values = parseSelect();
        } else {
            ExprList list;
            if (!check(TokenKind::RPAREN)) {
                do { list.push_back(parseExpression()); } while (match(TokenKind::COMMA));
            }
            e.values = std::move(list);
        }
        expect(TokenKind::RPAREN, ") after IN list");
        return parsePostfix(makeExpr<InExpr>(std::move(e)));
    }

    if (negated) {
        // We consumed NOT expecting LIKE — build a NOT(a LIKE b).
        TokenKind k = current().kind;   // KW_LIKE or KW_ILIKE
        advance();
        ast::Expr right = parseExpr(4 + 1);
        BinaryExpr be;
        be.op = (k == TokenKind::KW_ILIKE) ? BinaryOp::ILIKE : BinaryOp::LIKE;
        be.left = std::move(left);
        be.right = std::move(right);
        be.location = loc;
        UnaryExpr notE;
        notE.op = UnaryOp::NOT;
        notE.operand = makeExpr<BinaryExpr>(std::move(be));
        notE.location = loc;
        return parsePostfix(makeExpr<UnaryExpr>(std::move(notE)));
    }

    return left;
}

ast::Expr Parser::parsePrimary() {
    SourceLocation loc = current().location;
    const Token& t = current();

    switch (t.kind) {
        case TokenKind::LIT_INTEGER: {
            int64_t v = t.intValue(); advance();
            return makeExpr<Literal>(Literal::integer(v, loc));
        }
        case TokenKind::LIT_FLOAT: {
            double v = t.floatValue(); advance();
            return makeExpr<Literal>(Literal::real(v, loc));
        }
        case TokenKind::LIT_STRING: {
            std::string v = t.stringValue(); advance();
            return makeExpr<Literal>(Literal::string(std::move(v), loc));
        }
        case TokenKind::LIT_TRUE:  advance(); return makeExpr<Literal>(Literal::boolean(true, loc));
        case TokenKind::LIT_FALSE: advance(); return makeExpr<Literal>(Literal::boolean(false, loc));
        case TokenKind::LIT_NULL:  advance(); return makeExpr<Literal>(Literal::null(loc));

        case TokenKind::KW_CASE:   advance(); return parseCase(loc);

        case TokenKind::LPAREN: {
            advance();
            if (check(TokenKind::KW_SELECT)) {           // scalar subquery
                SubqueryExpr e; e.location = loc; e.subquery = parseSelect();
                expect(TokenKind::RPAREN, ") after subquery");
                return makeExpr<SubqueryExpr>(std::move(e));
            }
            ast::Expr inner = parseExpression();
            expect(TokenKind::RPAREN, ") to close expression");
            return inner;
        }

        case TokenKind::OP_STAR: {                       // COUNT(*) uses this too
            advance();
            ColumnRef star; star.column = "*"; star.location = loc;
            return makeExpr<ColumnRef>(std::move(star));
        }

        case TokenKind::IDENT: {
            std::string name = std::string(advance().lexeme);

            // Function call:  name '(' ...
            if (check(TokenKind::LPAREN))
                return parseFunctionCall(name, loc);

            // Qualified column: a.b or a.b.c
            ColumnRef col; col.location = loc;
            if (match(TokenKind::DOT)) {
                if (check(TokenKind::OP_STAR)) {         // t.*
                    advance();
                    col.table = name; col.column = "*";
                    return makeExpr<ColumnRef>(std::move(col));
                }
                std::string second = std::string(expect(TokenKind::IDENT, "column name after '.'").lexeme);
                if (match(TokenKind::DOT)) {
                    col.schema = name;
                    col.table  = second;
                    col.column = std::string(expect(TokenKind::IDENT, "column name after '.'").lexeme);
                } else {
                    col.table  = name;
                    col.column = second;
                }
            } else {
                col.column = name;
            }
            return makeExpr<ColumnRef>(std::move(col));
        }

        default:
            error("unexpected token '" + std::string(tokenKindName(t.kind)) +
                  "' in expression");
    }
}

ast::Expr Parser::parseFunctionCall(std::string name, SourceLocation loc) {
    expect(TokenKind::LPAREN, "( after function name");

    // Upper-case the function name (parser contract: FunctionCall.name is UPPER).
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    FunctionCall fn;
    fn.name     = name;
    fn.location = loc;

    if (match(TokenKind::KW_DISTINCT)) fn.distinct = true;

    if (check(TokenKind::OP_STAR)) {                 // COUNT(*)
        advance();
        fn.star = true;
    } else if (!check(TokenKind::RPAREN)) {
        do { fn.args.push_back(parseExpression()); } while (match(TokenKind::COMMA));
    }
    expect(TokenKind::RPAREN, ") to close function call");
    return makeExpr<FunctionCall>(std::move(fn));
}

ast::Expr Parser::parseCase(SourceLocation loc) {
    CaseExpr e;
    e.location = loc;

    // Optional subject: CASE <subject> WHEN ...
    if (!check(TokenKind::KW_WHEN))
        e.subject = parseExpression();

    while (match(TokenKind::KW_WHEN)) {
        CaseWhen w;
        w.condition = parseExpression();
        expect(TokenKind::KW_THEN, "THEN in CASE");
        w.result = parseExpression();
        e.whenClauses.push_back(std::move(w));
    }
    if (e.whenClauses.empty())
        error("CASE requires at least one WHEN clause");

    if (match(TokenKind::KW_ELSE))
        e.elseExpr = parseExpression();

    expect(TokenKind::KW_END, "END to close CASE");
    return makeExpr<CaseExpr>(std::move(e));
}

// ============================================================================
// Data types (for CAST and CREATE TABLE)
// ============================================================================

DataType Parser::parseDataType() {
    const Token& t = advance();
    switch (t.kind) {
        case TokenKind::KW_BOOLEAN:
        case TokenKind::KW_BOOL:    return DataType::boolean();
        case TokenKind::KW_TINYINT: return {TypeId::INT8};
        case TokenKind::KW_SMALLINT:return {TypeId::INT16};
        case TokenKind::KW_INT:
        case TokenKind::KW_INTEGER: return DataType::int32();
        case TokenKind::KW_BIGINT:  return DataType::int64();
        case TokenKind::KW_REAL:
        case TokenKind::KW_FLOAT:   return {TypeId::FLOAT32};
        case TokenKind::KW_DOUBLE:  return DataType::float64();
        case TokenKind::KW_DATE:    return DataType::date();
        case TokenKind::KW_TIMESTAMP: return DataType::timestamp();
        case TokenKind::KW_TEXT:    return DataType::varchar();
        case TokenKind::KW_VARCHAR: {
            int32_t len = -1;
            if (match(TokenKind::LPAREN)) {
                len = static_cast<int32_t>(expect(TokenKind::LIT_INTEGER, "length").intValue());
                expect(TokenKind::RPAREN, ") after VARCHAR length");
            }
            return DataType::varchar(len);
        }
        case TokenKind::KW_DECIMAL:
        case TokenKind::KW_NUMERIC: {
            uint8_t precision = 18, scale = 0;
            if (match(TokenKind::LPAREN)) {
                precision = static_cast<uint8_t>(expect(TokenKind::LIT_INTEGER, "precision").intValue());
                if (match(TokenKind::COMMA))
                    scale = static_cast<uint8_t>(expect(TokenKind::LIT_INTEGER, "scale").intValue());
                expect(TokenKind::RPAREN, ") after DECIMAL");
            }
            return DataType::decimal(precision, scale);
        }
        case TokenKind::KW_BLOB:    return {TypeId::BLOB};
        default:
            error("expected a data type name");
    }
}

// ============================================================================
// CAST(expr AS type)  — dispatched from parsePrimary via function name "CAST"
// (handled here so CREATE/INSERT can reuse parseDataType)
// ============================================================================

ast::Expr Parser::parseCast(SourceLocation loc) {
    // Reserved for the `::` cast shorthand and CAST(...) form. The FunctionCall
    // path already captures CAST(expr AS type) as a normal call is avoided;
    // CAST is parsed specially below in parsePrimary when needed.
    (void)loc;
    error("internal: parseCast not reachable");
}

// ============================================================================
// INSERT
// ============================================================================

ast::Stmt Parser::parseInsert() {
    SourceLocation loc = current().location;
    expect(TokenKind::KW_INSERT, "INSERT");
    expect(TokenKind::KW_INTO, "INTO after INSERT");

    InsertStmt ins;
    ins.location = loc;

    std::string first = std::string(expect(TokenKind::IDENT, "table name").lexeme);
    if (match(TokenKind::DOT)) {
        ins.schema = first;
        ins.table  = std::string(expect(TokenKind::IDENT, "table name after '.'").lexeme);
    } else {
        ins.table = first;
    }

    // Optional column list
    if (match(TokenKind::LPAREN)) {
        do {
            ins.columns.push_back(std::string(expect(TokenKind::IDENT, "column name").lexeme));
        } while (match(TokenKind::COMMA));
        expect(TokenKind::RPAREN, ") after column list");
    }

    if (check(TokenKind::KW_SELECT)) {
        ins.source = parseSelect();
    } else {
        expect(TokenKind::KW_VALUES, "VALUES or SELECT");
        std::vector<ExprList> rows;
        do {
            expect(TokenKind::LPAREN, "( to start a VALUES row");
            ExprList row;
            if (!check(TokenKind::RPAREN)) {
                do { row.push_back(parseExpression()); } while (match(TokenKind::COMMA));
            }
            expect(TokenKind::RPAREN, ") to close a VALUES row");
            rows.push_back(std::move(row));
        } while (match(TokenKind::COMMA));
        ins.source = std::move(rows);
    }

    return makeStmt<InsertStmt>(std::move(ins));
}

// ============================================================================
// CREATE TABLE
// ============================================================================

ast::Stmt Parser::parseCreateTable() {
    SourceLocation loc = current().location;
    expect(TokenKind::KW_CREATE, "CREATE");
    expect(TokenKind::KW_TABLE, "TABLE after CREATE");

    CreateTableStmt ct;
    ct.location = loc;

    // Note: `IF NOT EXISTS` is not supported — the lexer does not tokenize IF.

    std::string first = std::string(expect(TokenKind::IDENT, "table name").lexeme);
    if (match(TokenKind::DOT)) {
        ct.schema = first;
        ct.name   = std::string(expect(TokenKind::IDENT, "table name after '.'").lexeme);
    } else {
        ct.name = first;
    }

    expect(TokenKind::LPAREN, "( to start column definitions");
    do {
        ColumnSpec col;
        col.location = current().location;
        col.name     = std::string(expect(TokenKind::IDENT, "column name").lexeme);
        col.type     = parseDataType();

        // Column constraints
        for (;;) {
            if (match(TokenKind::KW_NOT)) {
                expect(TokenKind::LIT_NULL, "NULL after NOT");
                col.constraints.push_back({ColumnConstraint::Kind::NOT_NULL, std::nullopt, {}});
            } else if (match(TokenKind::KW_PRIMARY)) {
                expect(TokenKind::KW_KEY, "KEY after PRIMARY");
                col.constraints.push_back({ColumnConstraint::Kind::PRIMARY_KEY, std::nullopt, {}});
            } else if (match(TokenKind::KW_DEFAULT)) {
                ColumnConstraint c{ColumnConstraint::Kind::DEFAULT, std::nullopt, {}};
                c.expr = parseExpression();
                col.constraints.push_back(std::move(c));
            } else {
                break;
            }
        }
        ct.columns.push_back(std::move(col));
    } while (match(TokenKind::COMMA));
    expect(TokenKind::RPAREN, ") to close column definitions");

    return makeStmt<CreateTableStmt>(std::move(ct));
}

// ============================================================================
// DROP TABLE
// ============================================================================

ast::Stmt Parser::parseDropTable() {
    SourceLocation loc = current().location;
    expect(TokenKind::KW_DROP, "DROP");
    expect(TokenKind::KW_TABLE, "TABLE after DROP");

    DropTableStmt dt;
    dt.location = loc;

    // Note: `IF EXISTS` is not supported — the lexer does not tokenize IF.

    std::string first = std::string(expect(TokenKind::IDENT, "table name").lexeme);
    if (match(TokenKind::DOT)) {
        dt.schema = first;
        dt.name   = std::string(expect(TokenKind::IDENT, "table name after '.'").lexeme);
    } else {
        dt.name = first;
    }
    return makeStmt<DropTableStmt>(std::move(dt));
}

}  // namespace lq
