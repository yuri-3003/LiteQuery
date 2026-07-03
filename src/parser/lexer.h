#pragma once

// ============================================================================
// LiteQuery — lexer.h
// SQL tokenizer: source text → flat vector<Token>.
//
// Design principles:
//   - The lexer produces a *flat* token stream in one pass (Lexer::tokenize()).
//     There is no lazy/streaming interface — analytical SQL statements are
//     short, and a materialised vector<Token> keeps the parser simple and the
//     hot path allocation-free after the initial reserve().
//   - Token::lexeme is a std::string_view into the original source buffer, so
//     the source string MUST outlive every Token produced from it. The parser
//     copies out any text it needs to retain (identifiers, string contents).
//   - Errors are collected into LexResult::errors rather than thrown, so a
//     single malformed query still yields a usable (partial) token stream and
//     all lexical errors at once.
//   - Every Token carries a SourceLocation for precise diagnostics.
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lq {

// ============================================================================
// SourceLocation — 1-based line/column into the original SQL text
// ============================================================================

struct SourceLocation {
    uint32_t line   = 0;
    uint32_t column = 0;

    bool operator==(const SourceLocation& o) const noexcept {
        return line == o.line && column == o.column;
    }

    std::string toString() const {
        return std::to_string(line) + ":" + std::to_string(column);
    }
};

// ============================================================================
// TokenKind — every lexical category the tokenizer can emit
//
// Ordering is not significant, but the ranges are grouped for readability:
// literals, identifiers, keywords, operators, punctuation, and sentinels.
// ============================================================================

enum class TokenKind : uint16_t {
    // ---- Literals ----------------------------------------------------------
    LIT_INTEGER,
    LIT_FLOAT,
    LIT_STRING,
    LIT_TRUE,
    LIT_FALSE,
    LIT_NULL,

    // ---- Identifier --------------------------------------------------------
    IDENT,

    // ---- Keywords: statement / clause -------------------------------------
    KW_SELECT, KW_FROM, KW_WHERE, KW_GROUP, KW_BY, KW_HAVING,
    KW_ORDER, KW_LIMIT, KW_OFFSET, KW_DISTINCT, KW_AS,
    KW_UNION, KW_ALL,

    // ---- Keywords: joins ---------------------------------------------------
    KW_JOIN, KW_INNER, KW_LEFT, KW_RIGHT, KW_FULL, KW_OUTER, KW_CROSS, KW_ON,

    // ---- Keywords: DDL / DML ----------------------------------------------
    KW_CREATE, KW_TABLE, KW_DROP, KW_INSERT, KW_INTO, KW_VALUES,
    KW_DELETE, KW_UPDATE, KW_SET,

    // ---- Keywords: types ---------------------------------------------------
    KW_INT, KW_INTEGER, KW_BIGINT, KW_SMALLINT, KW_TINYINT,
    KW_FLOAT, KW_DOUBLE, KW_REAL, KW_BOOLEAN, KW_BOOL,
    KW_VARCHAR, KW_TEXT, KW_DATE, KW_TIMESTAMP, KW_DECIMAL, KW_NUMERIC, KW_BLOB,

    // ---- Keywords: constraints --------------------------------------------
    KW_NOT, KW_NULL_KW, KW_PRIMARY, KW_KEY, KW_DEFAULT,

    // ---- Keywords: expression operators -----------------------------------
    KW_AND, KW_OR, KW_NOT_KW, KW_IN, KW_LIKE, KW_ILIKE, KW_BETWEEN,
    KW_IS, KW_EXISTS,

    // ---- Keywords: CASE ----------------------------------------------------
    KW_CASE, KW_WHEN, KW_THEN, KW_ELSE, KW_END,

    // ---- Keywords: ORDER BY modifiers -------------------------------------
    KW_ASC, KW_DESC, KW_NULLS, KW_FIRST, KW_LAST,

    // ---- Keywords: window functions ---------------------------------------
    KW_OVER, KW_PARTITION, KW_ROWS, KW_RANGE, KW_UNBOUNDED,
    KW_PRECEDING, KW_FOLLOWING, KW_CURRENT, KW_ROW,

    // ---- Operators ---------------------------------------------------------
    OP_PLUS, OP_MINUS, OP_STAR, OP_SLASH, OP_PERCENT, OP_CARET,
    OP_EQ, OP_NEQ, OP_LT, OP_LTE, OP_GT, OP_GTE,
    OP_CONCAT, OP_ARROW, OP_ARROW2,

    // ---- Punctuation -------------------------------------------------------
    LPAREN, RPAREN, LBRACKET, RBRACKET, COMMA, SEMICOLON,
    DOT, COLON, DOUBLE_COLON, QUESTION, DOLLAR_NUM,

    // ---- Sentinels ---------------------------------------------------------
    END_OF_FILE,
    ERROR,
};

// Human-readable name for a token kind (diagnostics / EXPLAIN).
std::string_view tokenKindName(TokenKind k) noexcept;

// ============================================================================
// Token — one lexical unit
//
// `lexeme` is a view into the source buffer. The typed accessors decode the
// lexeme on demand — the lexer stays allocation-free, and only the parser
// pays for the (rare) string/number decode when it actually needs the value.
// ============================================================================

struct Token {
    TokenKind        kind = TokenKind::ERROR;
    std::string_view lexeme;
    SourceLocation   location;

    bool is(TokenKind k) const noexcept { return kind == k; }

    bool isKeyword() const noexcept {
        return kind >= TokenKind::KW_SELECT && kind <= TokenKind::KW_ROW;
    }
    bool isLiteral() const noexcept {
        return kind >= TokenKind::LIT_INTEGER && kind <= TokenKind::LIT_NULL;
    }
    bool isEof() const noexcept { return kind == TokenKind::END_OF_FILE; }

    // Decode the lexeme into its typed value. Each throws std::logic_error if
    // called on a token of the wrong kind, and a range/format error if the
    // lexeme cannot be parsed.
    std::string stringValue() const;   // LIT_STRING — quotes stripped, '' → '
    int64_t     intValue()    const;   // LIT_INTEGER
    double      floatValue()  const;   // LIT_FLOAT
};

// ============================================================================
// LexError — a single lexical error with its location
// ============================================================================

struct LexError {
    std::string    message;
    SourceLocation location;
};

// ============================================================================
// LexResult — full output of a tokenize() pass
//
// `tokens` always ends with an END_OF_FILE token, even on error, so the parser
// can rely on a terminator. `errors` is empty on success.
// ============================================================================

struct LexResult {
    std::vector<Token>    tokens;
    std::vector<LexError> errors;

    bool ok() const noexcept { return errors.empty(); }
};

// ============================================================================
// Lexer — single-pass tokenizer
//
// Usage:
//   Lexer lex(sql);
//   LexResult r = lex.tokenize();
//   if (!r.ok()) { report r.errors; }
// ============================================================================

class Lexer {
public:
    explicit Lexer(std::string_view source) noexcept;

    // Tokenize the entire source into a fresh LexResult.
    LexResult tokenize();

    // When true (the default) SQL comments (-- line, /* block */) are skipped.
    void setSkipComments(bool skip) noexcept { skipComments_ = skip; }

private:
    // ---- Cursor helpers ----------------------------------------------------
    char           peek(int offset = 0) const noexcept;
    char           advance() noexcept;
    bool           match(char expected) noexcept;
    bool           matchStr(std::string_view s) noexcept;
    bool           isAtEnd() const noexcept;
    SourceLocation currentLocation() const noexcept;
    Token          makeToken(TokenKind kind, size_t startPos) const noexcept;

    // ---- Skipping ----------------------------------------------------------
    void skipWhitespace() noexcept;
    void skipLineComment() noexcept;
    void skipBlockComment(LexResult& out);

    // ---- Token scanners ----------------------------------------------------
    void      scanNumber(LexResult& out);
    void      scanString(LexResult& out);
    void      scanQuotedIdent(LexResult& out);
    void      scanIdentOrKeyword(LexResult& out);
    void      scanDollarParam(LexResult& out);
    TokenKind classifyKeyword(std::string_view upper) const noexcept;

    std::string_view source_;
    size_t           pos_    = 0;
    uint32_t         line_   = 1;
    uint32_t         column_ = 1;
    bool             skipComments_ = true;
};

}  // namespace lq
