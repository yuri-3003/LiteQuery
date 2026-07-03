// ============================================================================
// LiteQuery — lexer.cpp
// ============================================================================

#include "lexer.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace lq {

// ============================================================================
// tokenKindName
// ============================================================================

std::string_view tokenKindName(TokenKind k) noexcept {
    switch (k) {
        case TokenKind::LIT_INTEGER:    return "LIT_INTEGER";
        case TokenKind::LIT_FLOAT:      return "LIT_FLOAT";
        case TokenKind::LIT_STRING:     return "LIT_STRING";
        case TokenKind::LIT_TRUE:       return "LIT_TRUE";
        case TokenKind::LIT_FALSE:      return "LIT_FALSE";
        case TokenKind::LIT_NULL:       return "LIT_NULL";
        case TokenKind::IDENT:          return "IDENT";
        case TokenKind::KW_SELECT:      return "SELECT";
        case TokenKind::KW_FROM:        return "FROM";
        case TokenKind::KW_WHERE:       return "WHERE";
        case TokenKind::KW_GROUP:       return "GROUP";
        case TokenKind::KW_BY:          return "BY";
        case TokenKind::KW_HAVING:      return "HAVING";
        case TokenKind::KW_ORDER:       return "ORDER";
        case TokenKind::KW_LIMIT:       return "LIMIT";
        case TokenKind::KW_OFFSET:      return "OFFSET";
        case TokenKind::KW_DISTINCT:    return "DISTINCT";
        case TokenKind::KW_AS:          return "AS";
        case TokenKind::KW_UNION:       return "UNION";
        case TokenKind::KW_ALL:         return "ALL";
        case TokenKind::KW_JOIN:        return "JOIN";
        case TokenKind::KW_INNER:       return "INNER";
        case TokenKind::KW_LEFT:        return "LEFT";
        case TokenKind::KW_RIGHT:       return "RIGHT";
        case TokenKind::KW_FULL:        return "FULL";
        case TokenKind::KW_OUTER:       return "OUTER";
        case TokenKind::KW_CROSS:       return "CROSS";
        case TokenKind::KW_ON:          return "ON";
        case TokenKind::KW_CREATE:      return "CREATE";
        case TokenKind::KW_TABLE:       return "TABLE";
        case TokenKind::KW_DROP:        return "DROP";
        case TokenKind::KW_INSERT:      return "INSERT";
        case TokenKind::KW_INTO:        return "INTO";
        case TokenKind::KW_VALUES:      return "VALUES";
        case TokenKind::KW_DELETE:      return "DELETE";
        case TokenKind::KW_UPDATE:      return "UPDATE";
        case TokenKind::KW_SET:         return "SET";
        case TokenKind::KW_INT:         return "INT";
        case TokenKind::KW_INTEGER:     return "INTEGER";
        case TokenKind::KW_BIGINT:      return "BIGINT";
        case TokenKind::KW_SMALLINT:    return "SMALLINT";
        case TokenKind::KW_TINYINT:     return "TINYINT";
        case TokenKind::KW_FLOAT:       return "FLOAT";
        case TokenKind::KW_DOUBLE:      return "DOUBLE";
        case TokenKind::KW_REAL:        return "REAL";
        case TokenKind::KW_BOOLEAN:     return "BOOLEAN";
        case TokenKind::KW_BOOL:        return "BOOL";
        case TokenKind::KW_VARCHAR:     return "VARCHAR";
        case TokenKind::KW_TEXT:        return "TEXT";
        case TokenKind::KW_DATE:        return "DATE";
        case TokenKind::KW_TIMESTAMP:   return "TIMESTAMP";
        case TokenKind::KW_DECIMAL:     return "DECIMAL";
        case TokenKind::KW_NUMERIC:     return "NUMERIC";
        case TokenKind::KW_BLOB:        return "BLOB";
        case TokenKind::KW_NOT:         return "NOT";
        case TokenKind::KW_NULL_KW:     return "NULL";
        case TokenKind::KW_PRIMARY:     return "PRIMARY";
        case TokenKind::KW_KEY:         return "KEY";
        case TokenKind::KW_DEFAULT:     return "DEFAULT";
        case TokenKind::KW_AND:         return "AND";
        case TokenKind::KW_OR:          return "OR";
        case TokenKind::KW_NOT_KW:      return "NOT";
        case TokenKind::KW_IN:          return "IN";
        case TokenKind::KW_LIKE:        return "LIKE";
        case TokenKind::KW_ILIKE:       return "ILIKE";
        case TokenKind::KW_BETWEEN:     return "BETWEEN";
        case TokenKind::KW_IS:          return "IS";
        case TokenKind::KW_EXISTS:      return "EXISTS";
        case TokenKind::KW_CASE:        return "CASE";
        case TokenKind::KW_WHEN:        return "WHEN";
        case TokenKind::KW_THEN:        return "THEN";
        case TokenKind::KW_ELSE:        return "ELSE";
        case TokenKind::KW_END:         return "END";
        case TokenKind::KW_ASC:         return "ASC";
        case TokenKind::KW_DESC:        return "DESC";
        case TokenKind::KW_NULLS:       return "NULLS";
        case TokenKind::KW_FIRST:       return "FIRST";
        case TokenKind::KW_LAST:        return "LAST";
        case TokenKind::KW_OVER:        return "OVER";
        case TokenKind::KW_PARTITION:   return "PARTITION";
        case TokenKind::KW_ROWS:        return "ROWS";
        case TokenKind::KW_RANGE:       return "RANGE";
        case TokenKind::KW_UNBOUNDED:   return "UNBOUNDED";
        case TokenKind::KW_PRECEDING:   return "PRECEDING";
        case TokenKind::KW_FOLLOWING:   return "FOLLOWING";
        case TokenKind::KW_CURRENT:     return "CURRENT";
        case TokenKind::KW_ROW:         return "ROW";
        case TokenKind::OP_PLUS:        return "+";
        case TokenKind::OP_MINUS:       return "-";
        case TokenKind::OP_STAR:        return "*";
        case TokenKind::OP_SLASH:       return "/";
        case TokenKind::OP_PERCENT:     return "%";
        case TokenKind::OP_CARET:       return "^";
        case TokenKind::OP_EQ:          return "=";
        case TokenKind::OP_NEQ:         return "<>";
        case TokenKind::OP_LT:          return "<";
        case TokenKind::OP_LTE:         return "<=";
        case TokenKind::OP_GT:          return ">";
        case TokenKind::OP_GTE:         return ">=";
        case TokenKind::OP_CONCAT:      return "||";
        case TokenKind::OP_ARROW:       return "->";
        case TokenKind::OP_ARROW2:      return "->>";
        case TokenKind::LPAREN:         return "(";
        case TokenKind::RPAREN:         return ")";
        case TokenKind::LBRACKET:       return "[";
        case TokenKind::RBRACKET:       return "]";
        case TokenKind::COMMA:          return ",";
        case TokenKind::SEMICOLON:      return ";";
        case TokenKind::DOT:            return ".";
        case TokenKind::COLON:          return ":";
        case TokenKind::DOUBLE_COLON:   return "::";
        case TokenKind::QUESTION:       return "?";
        case TokenKind::DOLLAR_NUM:     return "DOLLAR_PARAM";
        case TokenKind::END_OF_FILE:    return "EOF";
        case TokenKind::ERROR:          return "ERROR";
    }
    return "UNKNOWN";
}

// ============================================================================
// Token methods
// ============================================================================

std::string Token::stringValue() const {
    if (kind != TokenKind::LIT_STRING)
        throw std::logic_error("Token::stringValue called on non-string token");

    // lexeme is 'content', strip outer quotes
    assert(lexeme.size() >= 2 && lexeme.front() == '\'' && lexeme.back() == '\'');
    std::string_view inner = lexeme.substr(1, lexeme.size() - 2);

    std::string result;
    result.reserve(inner.size());
    for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '\'' && i + 1 < inner.size() && inner[i + 1] == '\'') {
            result += '\'';
            ++i;   // Skip the second quote
        } else {
            result += inner[i];
        }
    }
    return result;
}

int64_t Token::intValue() const {
    if (kind != TokenKind::LIT_INTEGER)
        throw std::logic_error("Token::intValue called on non-integer token");
    int64_t v = 0;
    auto [ptr, ec] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), v);
    if (ec != std::errc{})
        throw std::out_of_range("Integer literal out of range: " + std::string(lexeme));
    return v;
}

double Token::floatValue() const {
    if (kind != TokenKind::LIT_FLOAT)
        throw std::logic_error("Token::floatValue called on non-float token");
    double v = 0.0;
    // std::from_chars for float requires C++17 and not all stdlibs support it
    // for floats yet — fall back to stod with a null-terminated copy.
    std::string tmp(lexeme);
    size_t consumed = 0;
    v = std::stod(tmp, &consumed);
    if (consumed != tmp.size())
        throw std::invalid_argument("Malformed float literal: " + tmp);
    return v;
}

// ============================================================================
// Keyword lookup table — built once, searched in O(1)
// ============================================================================

static const std::unordered_map<std::string_view, TokenKind>& keywordTable() {
    static const std::unordered_map<std::string_view, TokenKind> table = {
        {"SELECT",     TokenKind::KW_SELECT},
        {"FROM",       TokenKind::KW_FROM},
        {"WHERE",      TokenKind::KW_WHERE},
        {"GROUP",      TokenKind::KW_GROUP},
        {"BY",         TokenKind::KW_BY},
        {"HAVING",     TokenKind::KW_HAVING},
        {"ORDER",      TokenKind::KW_ORDER},
        {"LIMIT",      TokenKind::KW_LIMIT},
        {"OFFSET",     TokenKind::KW_OFFSET},
        {"DISTINCT",   TokenKind::KW_DISTINCT},
        {"AS",         TokenKind::KW_AS},
        {"UNION",      TokenKind::KW_UNION},
        {"ALL",        TokenKind::KW_ALL},
        {"JOIN",       TokenKind::KW_JOIN},
        {"INNER",      TokenKind::KW_INNER},
        {"LEFT",       TokenKind::KW_LEFT},
        {"RIGHT",      TokenKind::KW_RIGHT},
        {"FULL",       TokenKind::KW_FULL},
        {"OUTER",      TokenKind::KW_OUTER},
        {"CROSS",      TokenKind::KW_CROSS},
        {"ON",         TokenKind::KW_ON},
        {"CREATE",     TokenKind::KW_CREATE},
        {"TABLE",      TokenKind::KW_TABLE},
        {"DROP",       TokenKind::KW_DROP},
        {"INSERT",     TokenKind::KW_INSERT},
        {"INTO",       TokenKind::KW_INTO},
        {"VALUES",     TokenKind::KW_VALUES},
        {"DELETE",     TokenKind::KW_DELETE},
        {"UPDATE",     TokenKind::KW_UPDATE},
        {"SET",        TokenKind::KW_SET},
        {"INT",        TokenKind::KW_INT},
        {"INTEGER",    TokenKind::KW_INTEGER},
        {"BIGINT",     TokenKind::KW_BIGINT},
        {"SMALLINT",   TokenKind::KW_SMALLINT},
        {"TINYINT",    TokenKind::KW_TINYINT},
        {"FLOAT",      TokenKind::KW_FLOAT},
        {"DOUBLE",     TokenKind::KW_DOUBLE},
        {"REAL",       TokenKind::KW_REAL},
        {"BOOLEAN",    TokenKind::KW_BOOLEAN},
        {"BOOL",       TokenKind::KW_BOOL},
        {"VARCHAR",    TokenKind::KW_VARCHAR},
        {"TEXT",       TokenKind::KW_TEXT},
        {"DATE",       TokenKind::KW_DATE},
        {"TIMESTAMP",  TokenKind::KW_TIMESTAMP},
        {"DECIMAL",    TokenKind::KW_DECIMAL},
        {"NUMERIC",    TokenKind::KW_NUMERIC},
        {"BLOB",       TokenKind::KW_BLOB},
        {"NOT",        TokenKind::KW_NOT},
        {"NULL",       TokenKind::LIT_NULL},    // NULL as a literal value
        {"PRIMARY",    TokenKind::KW_PRIMARY},
        {"KEY",        TokenKind::KW_KEY},
        {"DEFAULT",    TokenKind::KW_DEFAULT},
        {"AND",        TokenKind::KW_AND},
        {"OR",         TokenKind::KW_OR},
        {"IN",         TokenKind::KW_IN},
        {"LIKE",       TokenKind::KW_LIKE},
        {"ILIKE",      TokenKind::KW_ILIKE},
        {"BETWEEN",    TokenKind::KW_BETWEEN},
        {"IS",         TokenKind::KW_IS},
        {"EXISTS",     TokenKind::KW_EXISTS},
        {"CASE",       TokenKind::KW_CASE},
        {"WHEN",       TokenKind::KW_WHEN},
        {"THEN",       TokenKind::KW_THEN},
        {"ELSE",       TokenKind::KW_ELSE},
        {"END",        TokenKind::KW_END},
        {"ASC",        TokenKind::KW_ASC},
        {"DESC",       TokenKind::KW_DESC},
        {"NULLS",      TokenKind::KW_NULLS},
        {"FIRST",      TokenKind::KW_FIRST},
        {"LAST",       TokenKind::KW_LAST},
        {"OVER",       TokenKind::KW_OVER},
        {"PARTITION",  TokenKind::KW_PARTITION},
        {"ROWS",       TokenKind::KW_ROWS},
        {"RANGE",      TokenKind::KW_RANGE},
        {"UNBOUNDED",  TokenKind::KW_UNBOUNDED},
        {"PRECEDING",  TokenKind::KW_PRECEDING},
        {"FOLLOWING",  TokenKind::KW_FOLLOWING},
        {"CURRENT",    TokenKind::KW_CURRENT},
        {"ROW",        TokenKind::KW_ROW},
        {"TRUE",       TokenKind::LIT_TRUE},
        {"FALSE",      TokenKind::LIT_FALSE},
    };
    return table;
}

// ============================================================================
// Lexer — implementation
// ============================================================================

Lexer::Lexer(std::string_view source) noexcept
    : source_(source), pos_(0), line_(1), column_(1) {}

// ---- Core scanning helpers -------------------------------------------------

char Lexer::peek(int offset) const noexcept {
    size_t idx = pos_ + static_cast<size_t>(offset);
    return (idx < source_.size()) ? source_[idx] : '\0';
}

char Lexer::advance() noexcept {
    char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

bool Lexer::match(char expected) noexcept {
    if (isAtEnd() || source_[pos_] != expected) return false;
    advance();
    return true;
}

bool Lexer::matchStr(std::string_view s) noexcept {
    if (pos_ + s.size() > source_.size()) return false;
    if (source_.substr(pos_, s.size()) != s) return false;
    for (size_t i = 0; i < s.size(); ++i) advance();
    return true;
}

bool Lexer::isAtEnd() const noexcept {
    return pos_ >= source_.size();
}

SourceLocation Lexer::currentLocation() const noexcept {
    return {line_, column_};
}

Token Lexer::makeToken(TokenKind kind, size_t startPos) const noexcept {
    Token t;
    t.kind   = kind;
    t.lexeme = source_.substr(startPos, pos_ - startPos);
    // Location stored at start of token — recompute from startPos is complex;
    // callers capture location before they start scanning each token.
    return t;
}

// ---- Whitespace & comments -------------------------------------------------

void Lexer::skipWhitespace() noexcept {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else {
            break;
        }
    }
}

void Lexer::skipLineComment() noexcept {
    // Consume everything up to (but not including) the newline.
    while (!isAtEnd() && peek() != '\n')
        advance();
}

void Lexer::skipBlockComment(LexResult& out) {
    // We've already consumed '/*'; scan for '*/'. Track nesting depth so
    // nested block comments (/* /* */ */) work correctly.
    SourceLocation start = currentLocation();
    int depth = 1;
    while (!isAtEnd() && depth > 0) {
        char c = advance();
        if (c == '/' && peek() == '*') { advance(); ++depth; }
        else if (c == '*' && peek() == '/') { advance(); --depth; }
    }
    if (depth > 0) {
        out.errors.push_back({"Unterminated block comment", start});
    }
}

// ---- Number scanning -------------------------------------------------------

void Lexer::scanNumber(LexResult& out) {
    SourceLocation loc = currentLocation();
    size_t startPos    = pos_ - 1;  // Caller already advanced past first digit
    bool isFloat       = false;

    // Consume remaining digits
    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        advance();

    // Decimal part
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        isFloat = true;
        advance();  // consume '.'
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
            advance();
    }

    // Exponent: e/E followed by optional +/- and digits
    if (peek() == 'e' || peek() == 'E') {
        char next = peek(1);
        if (std::isdigit(static_cast<unsigned char>(next)) ||
            next == '+' || next == '-') {
            isFloat = true;
            advance();  // consume 'e'/'E'
            if (peek() == '+' || peek() == '-') advance();
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                out.errors.push_back({"Malformed exponent in numeric literal", loc});
                Token t = makeToken(TokenKind::ERROR, startPos);
                t.location = loc;
                out.tokens.push_back(t);
                return;
            }
            while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
                advance();
        }
    }

    Token t = makeToken(isFloat ? TokenKind::LIT_FLOAT : TokenKind::LIT_INTEGER,
                        startPos);
    t.location = loc;
    out.tokens.push_back(t);
}

// ---- String scanning -------------------------------------------------------

void Lexer::scanString(LexResult& out) {
    SourceLocation loc = currentLocation();
    size_t startPos    = pos_ - 1;  // include opening quote

    while (true) {
        if (isAtEnd()) {
            out.errors.push_back({"Unterminated string literal", loc});
            Token t = makeToken(TokenKind::ERROR, startPos);
            t.location = loc;
            out.tokens.push_back(t);
            return;
        }
        char c = advance();
        if (c == '\'') {
            if (peek() == '\'') {
                // Escaped quote '' — consume second quote and continue
                advance();
            } else {
                // Closing quote — done
                break;
            }
        }
    }

    Token t  = makeToken(TokenKind::LIT_STRING, startPos);
    t.location = loc;
    out.tokens.push_back(t);
}

// ---- Quoted identifier scanning --------------------------------------------

void Lexer::scanQuotedIdent(LexResult& out) {
    SourceLocation loc = currentLocation();
    size_t startPos    = pos_ - 1;

    while (true) {
        if (isAtEnd()) {
            out.errors.push_back({"Unterminated quoted identifier", loc});
            Token t = makeToken(TokenKind::ERROR, startPos);
            t.location = loc;
            out.tokens.push_back(t);
            return;
        }
        char c = advance();
        if (c == '"') {
            if (peek() == '"') {
                advance();  // "" escape inside quoted ident
            } else {
                break;      // closing "
            }
        }
    }

    Token t  = makeToken(TokenKind::IDENT, startPos);
    t.location = loc;
    out.tokens.push_back(t);
}

// ---- Identifier / keyword scanning -----------------------------------------

TokenKind Lexer::classifyKeyword(std::string_view upper) const noexcept {
    const auto& table = keywordTable();
    auto it = table.find(upper);
    return (it != table.end()) ? it->second : TokenKind::IDENT;
}

void Lexer::scanIdentOrKeyword(LexResult& out) {
    SourceLocation loc = currentLocation();
    size_t startPos    = pos_ - 1;  // Caller already consumed first char

    while (!isAtEnd()) {
        char c = peek();
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            advance();
        else
            break;
    }

    std::string_view raw = source_.substr(startPos, pos_ - startPos);

    // Build upper-case copy for case-insensitive keyword lookup.
    // Max SQL keyword length is ~11 chars; use stack buffer to avoid heap alloc.
    char upper[64];
    size_t len = std::min(raw.size(), sizeof(upper) - 1);
    for (size_t i = 0; i < len; ++i)
        upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(raw[i])));
    upper[len] = '\0';

    TokenKind kind = classifyKeyword(std::string_view(upper, len));

    Token t;
    t.kind     = kind;
    t.lexeme   = raw;
    t.location = loc;
    out.tokens.push_back(t);
}

// ---- Dollar parameter scanning ($1, $2) ------------------------------------

void Lexer::scanDollarParam(LexResult& out) {
    SourceLocation loc = currentLocation();
    size_t startPos    = pos_ - 1;

    if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        // Bare '$' with no digits — treat as an error
        out.errors.push_back({"Bare '$' is not a valid token; did you mean $1?", loc});
        Token t = makeToken(TokenKind::ERROR, startPos);
        t.location = loc;
        out.tokens.push_back(t);
        return;
    }
    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        advance();

    Token t  = makeToken(TokenKind::DOLLAR_NUM, startPos);
    t.location = loc;
    out.tokens.push_back(t);
}

// ============================================================================
// tokenize() — the main loop
// ============================================================================

LexResult Lexer::tokenize() {
    LexResult result;
    // Reserve a reasonable amount to avoid reallocation on typical queries.
    result.tokens.reserve(64);

    while (true) {
        skipWhitespace();

        if (isAtEnd()) break;

        SourceLocation loc = currentLocation();
        size_t startPos    = pos_;
        char c             = advance();

        // ---- Comments -------------------------------------------------------
        if (c == '-' && peek() == '-') {
            advance();  // second '-'
            if (skipComments_) {
                skipLineComment();
                continue;
            }
            // If not skipping, fall through — the '-' token was already
            // conceptually consumed; emit it (handled below as OP_MINUS).
            // For now we always skip comments in the lexer.
            skipLineComment();
            continue;
        }
        if (c == '/' && peek() == '*') {
            advance();  // '*'
            skipBlockComment(result);
            continue;
        }

        // ---- Single-character unambiguous tokens ----------------------------
        switch (c) {
            case '(':  { auto t = makeToken(TokenKind::LPAREN,    startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case ')':  { auto t = makeToken(TokenKind::RPAREN,    startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case '[':  { auto t = makeToken(TokenKind::LBRACKET,  startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case ']':  { auto t = makeToken(TokenKind::RBRACKET,  startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case ',':  { auto t = makeToken(TokenKind::COMMA,     startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case ';':  { auto t = makeToken(TokenKind::SEMICOLON, startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case '*':  { auto t = makeToken(TokenKind::OP_STAR,   startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case '+':  { auto t = makeToken(TokenKind::OP_PLUS,   startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case '%':  { auto t = makeToken(TokenKind::OP_PERCENT,startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case '^':  { auto t = makeToken(TokenKind::OP_CARET,  startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case '=':  { auto t = makeToken(TokenKind::OP_EQ,     startPos); t.location = loc; result.tokens.push_back(t); continue; }
            case '?':  { auto t = makeToken(TokenKind::QUESTION,  startPos); t.location = loc; result.tokens.push_back(t); continue; }
            default: break;
        }

        // ---- Multi-character operators --------------------------------------

        // < <= <>
        if (c == '<') {
            TokenKind k = TokenKind::OP_LT;
            if      (match('=')) k = TokenKind::OP_LTE;
            else if (match('>')) k = TokenKind::OP_NEQ;
            auto t = makeToken(k, startPos); t.location = loc;
            result.tokens.push_back(t); continue;
        }

        // > >=
        if (c == '>') {
            TokenKind k = match('=') ? TokenKind::OP_GTE : TokenKind::OP_GT;
            auto t = makeToken(k, startPos); t.location = loc;
            result.tokens.push_back(t); continue;
        }

        // != (alternative to <>)
        if (c == '!') {
            if (match('=')) {
                auto t = makeToken(TokenKind::OP_NEQ, startPos); t.location = loc;
                result.tokens.push_back(t); continue;
            }
            result.errors.push_back({"Unexpected '!'; did you mean '!='?", loc});
            auto t = makeToken(TokenKind::ERROR, startPos); t.location = loc;
            result.tokens.push_back(t); continue;
        }

        // | ||
        if (c == '|') {
            if (match('|')) {
                auto t = makeToken(TokenKind::OP_CONCAT, startPos); t.location = loc;
                result.tokens.push_back(t); continue;
            }
            result.errors.push_back({"Single '|' is not valid; did you mean '||'?", loc});
            auto t = makeToken(TokenKind::ERROR, startPos); t.location = loc;
            result.tokens.push_back(t); continue;
        }

        // - -> ->>
        if (c == '-') {
            if (peek() == '>') {
                advance();
                if (peek() == '>') {
                    advance();
                    auto t = makeToken(TokenKind::OP_ARROW2, startPos); t.location = loc;
                    result.tokens.push_back(t); continue;
                }
                auto t = makeToken(TokenKind::OP_ARROW, startPos); t.location = loc;
                result.tokens.push_back(t); continue;
            }
            auto t = makeToken(TokenKind::OP_MINUS, startPos); t.location = loc;
            result.tokens.push_back(t); continue;
        }

        // / (already handled block comment above — here it's division)
        if (c == '/') {
            auto t = makeToken(TokenKind::OP_SLASH, startPos); t.location = loc;
            result.tokens.push_back(t); continue;
        }

        // : ::
        if (c == ':') {
            TokenKind k = match(':') ? TokenKind::DOUBLE_COLON : TokenKind::COLON;
            auto t = makeToken(k, startPos); t.location = loc;
            result.tokens.push_back(t); continue;
        }

        // . (could be start of .5 float or qualified name separator)
        if (c == '.') {
            if (std::isdigit(static_cast<unsigned char>(peek()))) {
                // e.g. .5  →  treat as float literal starting with '.'
                // Put pos_ back one so scanNumber sees the full lexeme.
                // Actually: advance past digit and delegate.
                bool isFloat = true; (void)isFloat;
                while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
                    advance();
                Token t = makeToken(TokenKind::LIT_FLOAT, startPos);
                t.location = loc;
                result.tokens.push_back(t);
                continue;
            }
            auto t = makeToken(TokenKind::DOT, startPos); t.location = loc;
            result.tokens.push_back(t); continue;
        }

        // ---- Literals -------------------------------------------------------

        // String literal 'hello'
        if (c == '\'') { scanString(result); continue; }

        // Quoted identifier "name"
        if (c == '"') { scanQuotedIdent(result); continue; }

        // Numeric literal
        if (std::isdigit(static_cast<unsigned char>(c))) {
            scanNumber(result); continue;
        }

        // Dollar parameter $1
        if (c == '$') { scanDollarParam(result); continue; }

        // ---- Identifiers & keywords -----------------------------------------

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            scanIdentOrKeyword(result); continue;
        }

        // ---- Unknown character ----------------------------------------------

        std::string msg = "Unexpected character '";
        msg += c;
        msg += '\'';
        result.errors.push_back({msg, loc});
        Token errTok = makeToken(TokenKind::ERROR, startPos);
        errTok.location = loc;
        result.tokens.push_back(errTok);
    }

    // Always terminate with EOF
    Token eof;
    eof.kind     = TokenKind::END_OF_FILE;
    eof.lexeme   = source_.substr(pos_, 0);
    eof.location = currentLocation();
    result.tokens.push_back(eof);

    return result;
}

}  // namespace lq
