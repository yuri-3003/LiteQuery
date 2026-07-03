#pragma once

// ============================================================================
// LiteQuery — parser.h
// Recursive-descent / Pratt parser: token stream → AST (ast::StmtNode).
//
// Grammar covered (the analytical SQL subset LiteQuery executes):
//
//   statement   := select | insert | create_table | drop_table
//   select      := SELECT [DISTINCT] select_list
//                  [FROM from_list] [WHERE expr]
//                  [GROUP BY expr_list] [HAVING expr]
//                  [ORDER BY sort_list] [LIMIT n [OFFSET m]]
//                  [ (UNION [ALL]) select ]
//   from_list   := table_ref (',' table_ref)*
//   table_ref   := (name | '(' select ')') [AS? alias]
//                  ( [join_type] JOIN table_ref ON expr )*
//   expr        := Pratt-parsed: OR / AND / NOT / comparisons / +-*/ /
//                  unary +-  / IS [NOT] NULL / [NOT] BETWEEN / [NOT] IN /
//                  [NOT] LIKE / CASE / function calls / CAST / column refs /
//                  literals / parenthesised sub-expressions & scalar subqueries
//
// Errors are collected (multiple per parse) and also surfaced by throwing a
// ParseError from parse() when the statement cannot be recovered. The parser
// never reads past END_OF_FILE.
// ============================================================================

#include "ast.h"
#include "lexer.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace lq {

// ============================================================================
// ParseError — thrown when parsing cannot continue
// ============================================================================

struct ParseError : std::runtime_error {
    SourceLocation location;
    ParseError(const std::string& msg, SourceLocation loc)
        : std::runtime_error(msg + " (at " + loc.toString() + ")"), location(loc) {}
};

// ============================================================================
// Parser
//
// Usage:
//   Lexer lex(sql);
//   auto  toks = lex.tokenize();
//   Parser p(toks.tokens);
//   ast::Stmt stmt = p.parseStatement();   // throws ParseError on failure
// ============================================================================

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) noexcept;

    // Parse exactly one statement (the whole input). A trailing ';' is allowed.
    ast::Stmt parseStatement();

    // Parse a single SELECT (used for subqueries and set operations).
    std::unique_ptr<ast::SelectStmt> parseSelect();

    // Parse a standalone expression (used by tests and by DEFAULT/CHECK clauses).
    ast::Expr parseExpression();

private:
    // ---- Token cursor ------------------------------------------------------
    const Token& peek(int offset = 0) const noexcept;
    const Token& current() const noexcept;
    const Token& advance() noexcept;
    bool         check(TokenKind k) const noexcept;
    bool         match(TokenKind k) noexcept;         // consume if matches
    const Token& expect(TokenKind k, const char* what);
    bool         isAtEnd() const noexcept;

    [[noreturn]] void error(const std::string& msg) const;

    // ---- Statements --------------------------------------------------------
    ast::Stmt parseInsert();
    ast::Stmt parseCreateTable();
    ast::Stmt parseDropTable();

    // ---- SELECT clauses ----------------------------------------------------
    std::vector<ast::SelectItem> parseSelectList();
    ast::FromClause              parseFrom();
    ast::TableRefPtr             parseTableRef();
    ast::TableRefPtr             parseJoinChain(ast::TableRefPtr left);
    ast::TableRefPtr             parsePrimaryTableRef();
    std::vector<ast::SortKey>    parseOrderBy();

    // ---- Expressions (Pratt) ----------------------------------------------
    ast::Expr parseExpr(int minPrecedence);
    ast::Expr parseUnary();
    ast::Expr parsePrimary();
    ast::Expr parsePostfix(ast::Expr left);   // IS NULL / BETWEEN / IN / LIKE
    ast::Expr parseFunctionCall(std::string name, SourceLocation loc);
    ast::Expr parseCase(SourceLocation loc);
    ast::Expr parseCast(SourceLocation loc);

    // ---- Types (for CAST and CREATE TABLE) --------------------------------
    DataType parseDataType();

    const std::vector<Token>& tokens_;
    size_t                    pos_ = 0;
};

}  // namespace lq
