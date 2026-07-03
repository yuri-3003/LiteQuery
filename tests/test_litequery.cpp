// ============================================================================
// LiteQuery — test_litequery.cpp
// End-to-end and unit tests for the engine, using the C++ API.
// ============================================================================

#include "test_framework.h"

#include "lexer.h"
#include "parser.h"
#include "eval.h"
#include "connection.h"

#include <string>

using namespace lq;

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

static Connection makeDb() {
    Connection c;
    c.query("CREATE TABLE emp (id INT, name VARCHAR, dept VARCHAR, salary DOUBLE)");
    c.query("INSERT INTO emp VALUES "
            "(1,'Ann','Eng',100.0),(2,'Bob','Eng',120.0),"
            "(3,'Cy','Sales',90.0),(4,'Di','Sales',95.0),(5,'Ed','Eng',110.0)");
    return c;
}

// Fetch a single scalar (row 0, col 0) as double.
static double scalar(Connection& c, const std::string& sql) {
    QueryResult r = c.query(sql);
    if (!r.ok() || r.rows.empty()) return -1e18;
    return r.rows[0][0].toDouble();
}

// ============================================================================
// Lexer
// ============================================================================

TEST(lexer_tokenizes_select) {
    Lexer lex("SELECT a, b FROM t WHERE a >= 10");
    LexResult r = lex.tokenize();
    CHECK(r.ok());
    CHECK(r.tokens.front().kind == TokenKind::KW_SELECT);
    CHECK(r.tokens.back().kind == TokenKind::END_OF_FILE);
}

TEST(lexer_numbers_and_strings) {
    Lexer lex("3.14 42 'hi ''there'''");
    LexResult r = lex.tokenize();
    CHECK(r.ok());
    CHECK(r.tokens[0].kind == TokenKind::LIT_FLOAT);
    CHECK(r.tokens[1].kind == TokenKind::LIT_INTEGER);
    CHECK(r.tokens[2].kind == TokenKind::LIT_STRING);
    CHECK_EQ(r.tokens[1].intValue(), (int64_t)42);
    CHECK_EQ(r.tokens[2].stringValue(), std::string("hi 'there'"));
}

TEST(lexer_operators) {
    Lexer lex("<= >= <> != || -> ->>");
    LexResult r = lex.tokenize();
    CHECK(r.ok());
    CHECK(r.tokens[0].kind == TokenKind::OP_LTE);
    CHECK(r.tokens[1].kind == TokenKind::OP_GTE);
    CHECK(r.tokens[2].kind == TokenKind::OP_NEQ);
    CHECK(r.tokens[3].kind == TokenKind::OP_NEQ);
    CHECK(r.tokens[4].kind == TokenKind::OP_CONCAT);
}

// ============================================================================
// Parser
// ============================================================================

TEST(parser_select_roundtrip) {
    Lexer lex("SELECT a, b AS x FROM t WHERE a > 1 ORDER BY b DESC LIMIT 5");
    auto toks = lex.tokenize();
    Parser p(toks.tokens);
    ast::Stmt stmt = p.parseStatement();
    const auto* sel = std::get_if<ast::SelectStmt>(stmt.get());
    CHECK(sel != nullptr);
    CHECK_EQ(sel->selectList.size(), (size_t)2);
    CHECK(sel->selectList[1].alias.has_value());
    CHECK(sel->where.has_value());
    CHECK(sel->orderBy.has_value());
    CHECK(sel->limit.has_value());
}

TEST(parser_precedence) {
    // 1 + 2 * 3 should parse as 1 + (2*3)
    Lexer lex("SELECT 1 + 2 * 3");
    auto toks = lex.tokenize();
    Parser p(toks.tokens);
    ast::Stmt stmt = p.parseStatement();
    const auto* sel = std::get_if<ast::SelectStmt>(stmt.get());
    CHECK(sel != nullptr);
    const auto* bin = std::get_if<ast::BinaryExpr>(sel->selectList[0].expr.get());
    CHECK(bin != nullptr);
    CHECK(bin->op == ast::BinaryOp::ADD);   // top-level op is +, not *
}

TEST(parser_reports_errors) {
    Lexer lex("SELECT FROM");   // missing select list expression
    auto toks = lex.tokenize();
    Parser p(toks.tokens);
    bool threw = false;
    try { p.parseStatement(); } catch (const ParseError&) { threw = true; }
    CHECK(threw);
}

// ============================================================================
// Evaluator
// ============================================================================

static Value evalSql(const std::string& expr) {
    // The SQL string must outlive the Lexer: Token::lexeme (and the Lexer's
    // source_) are string_views into it. Keep it in a named local.
    std::string sql = "SELECT " + expr;
    Lexer lex(sql);
    auto toks = lex.tokenize();
    Parser p(toks.tokens);
    ast::Stmt stmt = p.parseStatement();
    const auto* sel = std::get_if<ast::SelectStmt>(stmt.get());
    return evaluate(*sel->selectList[0].expr, Schema{}, Row{});
}

TEST(eval_arithmetic) {
    CHECK_EQ(evalSql("2 + 3 * 4").toInt64(), (int64_t)14);
    CHECK_EQ(evalSql("10 / 4").toDouble(), 2.5);
    CHECK_EQ(evalSql("10 % 3").toInt64(), (int64_t)1);
    CHECK_EQ(evalSql("-5 + 8").toInt64(), (int64_t)3);
}

TEST(eval_comparison_and_logic) {
    CHECK(evalSql("3 > 2 AND 1 < 2").getBool());
    CHECK(!evalSql("3 > 2 AND 2 < 1").getBool());
    CHECK(evalSql("1 = 1 OR 1 = 2").getBool());
    CHECK(evalSql("NOT (1 = 2)").getBool());
}

TEST(eval_null_semantics) {
    CHECK(evalSql("NULL + 1").isNull());
    CHECK(evalSql("NULL = NULL").isNull());
    // false AND NULL = false (definite)
    CHECK(evalSql("1 = 2 AND NULL").getBool() == false);
}

TEST(eval_string_functions) {
    CHECK_EQ(evalSql("UPPER('abc')").getString(), std::string("ABC"));
    CHECK_EQ(evalSql("LENGTH('hello')").toInt64(), (int64_t)5);
    CHECK_EQ(evalSql("COALESCE(NULL, NULL, 7)").toInt64(), (int64_t)7);
    CHECK_EQ(evalSql("'a' || 'b' || 'c'").getString(), std::string("abc"));
}

TEST(eval_like) {
    CHECK(likeMatch("hello", "h%o", false));
    CHECK(likeMatch("hello", "h_llo", false));
    CHECK(!likeMatch("hello", "h_lo", false));
    CHECK(likeMatch("ABC", "abc", true));   // ILIKE
}

TEST(eval_case) {
    CHECK_EQ(evalSql("CASE WHEN 1>2 THEN 10 ELSE 20 END").toInt64(), (int64_t)20);
    CHECK_EQ(evalSql("CASE 2 WHEN 1 THEN 'a' WHEN 2 THEN 'b' END").getString(), std::string("b"));
}

// ============================================================================
// Full pipeline — SELECT / WHERE / ORDER BY / LIMIT
// ============================================================================

TEST(select_star) {
    Connection c = makeDb();
    QueryResult r = c.query("SELECT * FROM emp");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)5);
    CHECK_EQ(r.schema.size(), (size_t)4);
}

TEST(select_where) {
    Connection c = makeDb();
    QueryResult r = c.query("SELECT name FROM emp WHERE salary > 100");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)2);   // Bob(120), Ed(110)
}

TEST(select_order_by_dropped_column) {
    // Regression: ORDER BY a column not in the SELECT list.
    Connection c = makeDb();
    QueryResult r = c.query("SELECT name FROM emp ORDER BY salary DESC");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)5);
    CHECK_EQ(r.rows[0][0].getString(), std::string("Bob"));   // 120
    CHECK_EQ(r.rows[4][0].getString(), std::string("Cy"));    // 90
}

TEST(select_limit_offset) {
    Connection c = makeDb();
    QueryResult r = c.query("SELECT name FROM emp ORDER BY salary DESC LIMIT 2 OFFSET 1");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)2);
    CHECK_EQ(r.rows[0][0].getString(), std::string("Ed"));    // 2nd highest
}

TEST(select_distinct) {
    Connection c = makeDb();
    QueryResult r = c.query("SELECT DISTINCT dept FROM emp");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)2);   // Eng, Sales
}

// ============================================================================
// Aggregation
// ============================================================================

TEST(aggregate_count_star) {
    Connection c = makeDb();
    CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM emp"), 5.0);
}

TEST(aggregate_sum_avg) {
    Connection c = makeDb();
    CHECK_EQ(scalar(c, "SELECT SUM(salary) FROM emp"), 515.0);
    CHECK_EQ(scalar(c, "SELECT AVG(salary) FROM emp"), 103.0);
    CHECK_EQ(scalar(c, "SELECT MIN(salary) FROM emp"), 90.0);
    CHECK_EQ(scalar(c, "SELECT MAX(salary) FROM emp"), 120.0);
}

TEST(group_by) {
    Connection c = makeDb();
    QueryResult r = c.query("SELECT dept, COUNT(*), SUM(salary) FROM emp GROUP BY dept");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)2);
    // Find the Eng group.
    double engSum = -1;
    for (auto& row : r.rows)
        if (row[0].getString() == "Eng") engSum = row[2].toDouble();
    CHECK_EQ(engSum, 330.0);   // 100+120+110
}

// ============================================================================
// Joins
// ============================================================================

static Connection makeJoinDb() {
    Connection c;
    c.query("CREATE TABLE d (id INT, dname VARCHAR)");
    c.query("INSERT INTO d VALUES (1,'Eng'),(2,'Sales')");
    c.query("CREATE TABLE e (id INT, name VARCHAR, did INT)");
    c.query("INSERT INTO e VALUES (10,'Ann',1),(11,'Bob',1),(12,'Cy',2),(13,'Zed',9)");
    return c;
}

TEST(inner_join) {
    Connection c = makeJoinDb();
    QueryResult r = c.query(
        "SELECT e.name, d.dname FROM e AS e JOIN d AS d ON e.did = d.id ORDER BY e.name");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)3);   // Zed unmatched
    CHECK_EQ(r.rows[0][0].getString(), std::string("Ann"));
}

TEST(left_join_keeps_unmatched) {
    Connection c = makeJoinDb();
    QueryResult r = c.query(
        "SELECT e.name, d.dname FROM e AS e LEFT JOIN d AS d ON e.did = d.id ORDER BY e.name");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)4);
    // Zed's department is NULL.
    for (auto& row : r.rows)
        if (row[0].getString() == "Zed") CHECK(row[1].isNull());
}

// ============================================================================
// DDL / DML & error handling
// ============================================================================

TEST(insert_reports_affected) {
    Connection c;
    c.query("CREATE TABLE t (a INT)");
    QueryResult r = c.query("INSERT INTO t VALUES (1),(2),(3)");
    CHECK(r.ok());
    CHECK_EQ(r.rowsAffected, (int64_t)3);
}

TEST(unknown_table_is_error_not_crash) {
    Connection c;
    QueryResult r = c.query("SELECT * FROM does_not_exist");
    CHECK(!r.ok());
    CHECK(!r.errorMessage.empty());
}

TEST(syntax_error_is_reported) {
    Connection c;
    QueryResult r = c.query("SELECT FROM WHERE");
    CHECK(!r.ok());
}

TEST(drop_table) {
    Connection c;
    c.query("CREATE TABLE t (a INT)");
    CHECK(c.query("SELECT * FROM t").ok());
    c.query("DROP TABLE t");
    CHECK(!c.query("SELECT * FROM t").ok());
}

int main() {
    std::printf("LiteQuery test suite\n====================\n");
    return lqtest::run_all();
}
