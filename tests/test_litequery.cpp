// ============================================================================
// LiteQuery — test_litequery.cpp
// End-to-end and unit tests for the engine, using the C++ API.
// ============================================================================

#include "test_framework.h"

#include "lexer.h"
#include "parser.h"
#include "eval.h"
#include "connection.h"
#include "csv_reader.h"

#include <cstdio>
#include <fstream>
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

TEST(group_by_order_by_group_key) {
    // Regression: ORDER BY a group key must sort the aggregate result.
    Connection c = makeDb();
    QueryResult r = c.query("SELECT dept, COUNT(*) FROM emp GROUP BY dept ORDER BY dept");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)2);
    CHECK_EQ(r.rows[0][0].getString(), std::string("Eng"));   // Eng < Sales
    CHECK_EQ(r.rows[1][0].getString(), std::string("Sales"));
}

TEST(group_by_order_by_aggregate_alias) {
    // Regression: ORDER BY an aliased aggregate result + LIMIT.
    Connection c = makeDb();
    QueryResult r = c.query(
        "SELECT dept, SUM(salary) AS total FROM emp GROUP BY dept ORDER BY total DESC LIMIT 1");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)1);
    CHECK_EQ(r.rows[0][0].getString(), std::string("Eng"));   // 330 > 185
    CHECK_EQ(r.rows[0][1].toDouble(), 330.0);
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

// ============================================================================
// SQL completeness — HAVING, ORDER BY aggregates, UNION, INSERT..SELECT
// ============================================================================

TEST(having_with_aggregate_expression) {
    Connection c = makeDb();
    // Eng sum=330, Sales sum=185 → only Eng passes.
    QueryResult r = c.query(
        "SELECT dept, SUM(salary) FROM emp GROUP BY dept HAVING SUM(salary) > 200");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)1);
    CHECK_EQ(r.rows[0][0].getString(), std::string("Eng"));
    CHECK_EQ(r.rows[0][1].toDouble(), 330.0);
}

TEST(having_aggregate_not_in_select) {
    Connection c = makeDb();
    // HAVING references COUNT(*) which is not in the SELECT list.
    QueryResult r = c.query(
        "SELECT dept FROM emp GROUP BY dept HAVING COUNT(*) > 2");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)1);            // only Eng has 3 people
    CHECK_EQ(r.rows[0][0].getString(), std::string("Eng"));
    CHECK_EQ(r.schema.size(), (size_t)1);          // COUNT must NOT leak into output
}

TEST(order_by_bare_aggregate) {
    Connection c = makeDb();
    QueryResult r = c.query(
        "SELECT dept, SUM(salary) FROM emp GROUP BY dept ORDER BY SUM(salary) DESC");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)2);
    CHECK_EQ(r.rows[0][0].getString(), std::string("Eng"));    // 330 first
    CHECK_EQ(r.rows[1][0].getString(), std::string("Sales"));  // 185 second
}

TEST(select_expression_over_aggregates) {
    Connection c;
    c.query("CREATE TABLE t (v INT)");
    c.query("INSERT INTO t VALUES (10),(20),(30)");
    // A computed expression combining two aggregates.
    CHECK_EQ(scalar(c, "SELECT SUM(v) / COUNT(*) FROM t"), 20.0);
    CHECK_EQ(scalar(c, "SELECT MAX(v) - MIN(v) FROM t"), 20.0);
}

TEST(union_all_concatenates) {
    Connection c;
    c.query("CREATE TABLE a (x INT)");
    c.query("INSERT INTO a VALUES (1),(2)");
    c.query("CREATE TABLE b (x INT)");
    c.query("INSERT INTO b VALUES (2),(3)");
    QueryResult r = c.query("SELECT x FROM a UNION ALL SELECT x FROM b");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)4);            // 1,2,2,3 — duplicates kept
}

TEST(union_deduplicates) {
    Connection c;
    c.query("CREATE TABLE a (x INT)");
    c.query("INSERT INTO a VALUES (1),(2)");
    c.query("CREATE TABLE b (x INT)");
    c.query("INSERT INTO b VALUES (2),(3)");
    QueryResult r = c.query("SELECT x FROM a UNION SELECT x FROM b");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)3);            // 1,2,3 — the shared 2 deduped
}

TEST(union_column_count_mismatch_errors) {
    Connection c;
    c.query("CREATE TABLE a (x INT)");
    c.query("CREATE TABLE b (x INT, y INT)");
    QueryResult r = c.query("SELECT x FROM a UNION ALL SELECT x, y FROM b");
    CHECK(!r.ok());                                 // clean error, not silent misalignment
}

TEST(insert_select) {
    Connection c;
    c.query("CREATE TABLE src (x INT, y VARCHAR)");
    c.query("INSERT INTO src VALUES (1,'a'),(2,'b'),(3,'c')");
    c.query("CREATE TABLE dst (x INT, y VARCHAR)");
    QueryResult ins = c.query("INSERT INTO dst SELECT x, y FROM src WHERE x >= 2");
    CHECK(ins.ok());
    CHECK_EQ(ins.rowsAffected, (int64_t)2);
    QueryResult r = c.query("SELECT x, y FROM dst ORDER BY x");
    CHECK_EQ(r.rows.size(), (size_t)2);
    CHECK_EQ(r.rows[0][1].getString(), std::string("b"));
}

TEST(insert_select_count_mismatch_errors) {
    Connection c;
    c.query("CREATE TABLE src (x INT, y INT)");
    c.query("INSERT INTO src VALUES (1,2)");
    c.query("CREATE TABLE dst (x INT)");
    CHECK(!c.query("INSERT INTO dst SELECT x, y FROM src").ok());
}

// ---- UPDATE / DELETE -------------------------------------------------------

TEST(update_with_where) {
    Connection c;
    c.query("CREATE TABLE t (id INT, sal DOUBLE)");
    c.query("INSERT INTO t VALUES (1,100),(2,200),(3,300)");
    QueryResult u = c.query("UPDATE t SET sal = 999 WHERE id = 2");
    CHECK(u.ok());
    CHECK_EQ(u.rowsAffected, (int64_t)1);
    QueryResult r = c.query("SELECT sal FROM t ORDER BY id");
    CHECK_EQ(r.rows[0][0].toDouble(), 100.0);
    CHECK_EQ(r.rows[1][0].toDouble(), 999.0);
    CHECK_EQ(r.rows[2][0].toDouble(), 300.0);
}

TEST(update_computed_sees_old_values) {
    // SET expressions reference the pre-update row (standard SQL).
    Connection c;
    c.query("CREATE TABLE t (x INT, y INT)");
    c.query("INSERT INTO t VALUES (1,10),(2,20)");
    QueryResult u = c.query("UPDATE t SET x = x + 100, y = y * 2");
    CHECK(u.ok());
    CHECK_EQ(u.rowsAffected, (int64_t)2);       // no WHERE → all rows
    QueryResult r = c.query("SELECT x, y FROM t ORDER BY x");
    CHECK_EQ(r.rows[0][0].toInt64(), (int64_t)101);
    CHECK_EQ(r.rows[0][1].toInt64(), (int64_t)20);
    CHECK_EQ(r.rows[1][0].toInt64(), (int64_t)102);
    CHECK_EQ(r.rows[1][1].toInt64(), (int64_t)40);
}

TEST(update_to_null) {
    Connection c;
    c.query("CREATE TABLE t (x INT, y VARCHAR)");
    c.query("INSERT INTO t VALUES (1,'a'),(2,'b')");
    c.query("UPDATE t SET y = NULL WHERE x = 1");
    QueryResult r = c.query("SELECT y FROM t ORDER BY x");
    CHECK(r.rows[0][0].isNull());
    CHECK_EQ(r.rows[1][0].getString(), std::string("b"));
}

TEST(update_unknown_column_errors) {
    Connection c;
    c.query("CREATE TABLE t (x INT)");
    c.query("INSERT INTO t VALUES (1)");
    CHECK(!c.query("UPDATE t SET nope = 5").ok());
}

TEST(delete_with_where) {
    Connection c;
    c.query("CREATE TABLE t (x INT)");
    c.query("INSERT INTO t VALUES (1),(2),(3),(4)");
    QueryResult d = c.query("DELETE FROM t WHERE x > 2");
    CHECK(d.ok());
    CHECK_EQ(d.rowsAffected, (int64_t)2);
    QueryResult r = c.query("SELECT COUNT(*) FROM t");
    CHECK_EQ(r.rows[0][0].toInt64(), (int64_t)2);
}

TEST(delete_all) {
    Connection c;
    c.query("CREATE TABLE t (x INT)");
    c.query("INSERT INTO t VALUES (1),(2),(3)");
    QueryResult d = c.query("DELETE FROM t");
    CHECK_EQ(d.rowsAffected, (int64_t)3);
    CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM t"), 0.0);
    // Schema is intact — can still insert afterwards.
    c.query("INSERT INTO t VALUES (9)");
    CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM t"), 1.0);
}

TEST(update_then_persist) {
    // UPDATE/DELETE produce a rebuilt table; verify it still saves/loads.
    std::string path = "._lq_test_upd_persist.lqdb";
    {
        Connection c;
        c.query("CREATE TABLE t (id INT, v DOUBLE)");
        c.query("INSERT INTO t VALUES (1,10),(2,20),(3,30)");
        c.query("UPDATE t SET v = 0 WHERE id = 2");
        c.query("DELETE FROM t WHERE id = 3");
        CHECK(c.saveDatabase(path).ok());
    }
    {
        Connection c;
        CHECK(c.loadDatabase(path).ok());
        QueryResult r = c.query("SELECT id, v FROM t ORDER BY id");
        CHECK_EQ(r.rows.size(), (size_t)2);
        CHECK_EQ(r.rows[0][1].toDouble(), 10.0);
        CHECK_EQ(r.rows[1][1].toDouble(), 0.0);
    }
    std::remove(path.c_str());
}

// ============================================================================
// Persistence — save/load round-trips the whole catalog
// ============================================================================

TEST(persist_round_trip_all_types) {
    std::string path = "._lq_test_persist.lqdb";
    {
        Connection c;
        c.query("CREATE TABLE t (id INT, name VARCHAR, amt DOUBLE, ok BOOLEAN)");
        c.query("INSERT INTO t VALUES (1,'Ann',10.5,TRUE),(2,'Bob',20.0,FALSE),(3,'Cy',5.0,TRUE)");
        QueryResult s = c.saveDatabase(path);
        CHECK(s.ok());
    }
    {
        Connection c;   // fresh connection — nothing in memory
        QueryResult l = c.loadDatabase(path);
        CHECK(l.ok());
        QueryResult r = c.query("SELECT id, name, amt, ok FROM t ORDER BY id");
        CHECK(r.ok());
        CHECK_EQ(r.rows.size(), (size_t)3);
        CHECK_EQ(r.rows[0][1].getString(), std::string("Ann"));
        CHECK_EQ(r.rows[0][2].toDouble(), 10.5);
        CHECK_EQ(r.rows[1][1].getString(), std::string("Bob"));
        // Aggregate on reloaded data.
        CHECK_EQ(scalar(c, "SELECT SUM(amt) FROM t"), 35.5);
    }
    std::remove(path.c_str());
}

TEST(persist_preserves_nulls) {
    std::string path = "._lq_test_nulls.lqdb";
    {
        Connection c;
        c.query("CREATE TABLE t (id INT, v DOUBLE, s VARCHAR)");
        c.query("INSERT INTO t VALUES (1,10.0,'a'),(2,NULL,NULL),(3,30.0,'c')");
        CHECK(c.saveDatabase(path).ok());
    }
    {
        Connection c;
        CHECK(c.loadDatabase(path).ok());
        QueryResult r = c.query("SELECT id, v, s FROM t ORDER BY id");
        CHECK(r.rows[1][1].isNull());    // v NULL preserved
        CHECK(r.rows[1][2].isNull());    // s NULL preserved
        CHECK(!r.rows[0][1].isNull());
        // AVG skips the NULL: (10+30)/2 = 20
        CHECK_EQ(scalar(c, "SELECT AVG(v) FROM t"), 20.0);
    }
    std::remove(path.c_str());
}

TEST(persist_multiple_tables) {
    std::string path = "._lq_test_multi.lqdb";
    {
        Connection c;
        c.query("CREATE TABLE a (x INT)");
        c.query("INSERT INTO a VALUES (1),(2),(3)");
        c.query("CREATE TABLE b (y VARCHAR)");
        c.query("INSERT INTO b VALUES ('p'),('q')");
        QueryResult s = c.saveDatabase(path);
        CHECK(s.ok());
        CHECK_EQ(s.rowsAffected, (int64_t)2);   // 2 tables saved
    }
    {
        Connection c;
        c.loadDatabase(path);
        CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM a"), 3.0);
        CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM b"), 2.0);
    }
    std::remove(path.c_str());
}

TEST(persist_empty_table) {
    std::string path = "._lq_test_empty.lqdb";
    {
        Connection c;
        c.query("CREATE TABLE t (x INT, y VARCHAR)");   // no rows
        CHECK(c.saveDatabase(path).ok());
    }
    {
        Connection c;
        CHECK(c.loadDatabase(path).ok());
        QueryResult r = c.query("SELECT * FROM t");
        CHECK(r.ok());
        CHECK_EQ(r.rows.size(), (size_t)0);
        CHECK_EQ(r.schema.size(), (size_t)2);   // schema preserved
    }
    std::remove(path.c_str());
}

TEST(persist_bad_file_is_error) {
    // A non-database file must fail cleanly, not crash.
    std::string path = "._lq_test_garbage.lqdb";
    { std::ofstream f(path); f << "this is not a LiteQuery database file"; }
    Connection c;
    QueryResult r = c.loadDatabase(path);
    CHECK(!r.ok());
    CHECK(!r.errorMessage.empty());
    std::remove(path.c_str());

    // A missing file is also a clean error.
    CHECK(!c.loadDatabase("._no_such_db_file.lqdb").ok());
}

// ============================================================================
// Typed fast-aggregate path — must match the general path exactly
// ============================================================================

TEST(fast_agg_count_sum_avg_minmax) {
    Connection c = makeDb();
    CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM emp"), 5.0);
    CHECK_EQ(scalar(c, "SELECT SUM(salary) FROM emp"), 515.0);
    CHECK_EQ(scalar(c, "SELECT AVG(salary) FROM emp"), 103.0);
    CHECK_EQ(scalar(c, "SELECT MIN(salary) FROM emp"), 90.0);
    CHECK_EQ(scalar(c, "SELECT MAX(salary) FROM emp"), 120.0);
}

TEST(fast_agg_with_where) {
    Connection c = makeDb();
    // WHERE salary > 100 keeps Bob(120) and Ed(110): count 2, sum 230.
    CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM emp WHERE salary > 100"), 2.0);
    CHECK_EQ(scalar(c, "SELECT SUM(salary) FROM emp WHERE salary > 100"), 230.0);
    // Conjunction: dept = 'Eng' AND salary >= 110 → Bob, Ed.
    CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM emp WHERE dept = 'Eng' AND salary >= 110"), 2.0);
}

TEST(fast_agg_group_by_ordered) {
    Connection c = makeDb();
    QueryResult r = c.query(
        "SELECT dept, SUM(salary) AS total FROM emp GROUP BY dept ORDER BY total DESC");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)2);
    CHECK_EQ(r.rows[0][0].getString(), std::string("Eng"));   // 330 > 185
    CHECK_EQ(r.rows[0][1].toDouble(), 330.0);
    CHECK_EQ(r.rows[1][1].toDouble(), 185.0);
}

TEST(fast_agg_skips_nulls) {
    Connection c;
    c.query("CREATE TABLE t (v DOUBLE)");
    c.query("INSERT INTO t VALUES (10.0), (NULL), (20.0), (NULL)");
    CHECK_EQ(scalar(c, "SELECT COUNT(*) FROM t"), 4.0);      // COUNT(*) counts all
    CHECK_EQ(scalar(c, "SELECT COUNT(v) FROM t"), 2.0);      // COUNT(v) skips NULLs
    CHECK_EQ(scalar(c, "SELECT SUM(v) FROM t"), 30.0);
    CHECK_EQ(scalar(c, "SELECT AVG(v) FROM t"), 15.0);       // 30 / 2, not / 4
}

TEST(fast_agg_empty_table) {
    Connection c;
    c.query("CREATE TABLE t (v INT)");
    // Aggregate over no rows: COUNT → 0, SUM → NULL.
    QueryResult r = c.query("SELECT COUNT(*), SUM(v) FROM t");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)1);
    CHECK_EQ(r.rows[0][0].toInt64(), (int64_t)0);
    CHECK(r.rows[0][1].isNull());
}

TEST(fast_agg_matches_manual_group_by) {
    // Cross-check: build 6 groups and verify each SUM independently.
    Connection c;
    c.query("CREATE TABLE t (g INT, v INT)");
    c.query("INSERT INTO t VALUES (1,10),(2,20),(1,5),(3,7),(2,3),(1,1)");
    QueryResult r = c.query("SELECT g, SUM(v) FROM t GROUP BY g ORDER BY g");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)3);
    CHECK_EQ(r.rows[0][1].toDouble(), 16.0);   // g=1: 10+5+1
    CHECK_EQ(r.rows[1][1].toDouble(), 23.0);   // g=2: 20+3
    CHECK_EQ(r.rows[2][1].toDouble(), 7.0);    // g=3: 7
}

// ============================================================================
// CSV ingestion
// ============================================================================

TEST(csv_basic_inference) {
    TablePtr t = readCsvString("id,name,amt\n1,Ann,10.5\n2,Bob,20\n", "t");
    CHECK_EQ(t->rowCount(), (size_t)2);
    CHECK_EQ(t->columnCount(), (size_t)3);
    CHECK(t->schema()[0].type.id == TypeId::INT64);    // id
    CHECK(t->schema()[1].type.id == TypeId::VARCHAR);  // name
    CHECK(t->schema()[2].type.id == TypeId::FLOAT64);  // amt (10.5 forces double)
    CHECK_EQ(t->columnAt(1)[0].getString(), std::string("Ann"));
    CHECK_EQ(t->columnAt(2)[1].toDouble(), 20.0);
}

TEST(csv_quoted_fields) {
    // Quoted comma, and "" escape inside a quoted field.
    TablePtr t = readCsvString("a,b\n\"x, y\",\"he said \"\"hi\"\"\"\n", "t");
    CHECK_EQ(t->rowCount(), (size_t)1);
    CHECK_EQ(t->columnAt(0)[0].getString(), std::string("x, y"));
    CHECK_EQ(t->columnAt(1)[0].getString(), std::string("he said \"hi\""));
}

TEST(csv_empty_is_null) {
    // Blank cells become NULL and do not widen the numeric type.
    TablePtr t = readCsvString("n\n5\n\n7\n", "t");
    CHECK(t->schema()[0].type.id == TypeId::INT64);
    CHECK_EQ(t->rowCount(), (size_t)3);
    CHECK(t->columnAt(0)[1].isNull());
}

TEST(csv_crlf_and_bom) {
    // Leading UTF-8 BOM + CRLF line endings.
    std::string bom = "\xEF\xBB\xBF";
    TablePtr t = readCsvString(bom + "x,y\r\n1,2\r\n3,4\r\n", "t");
    CHECK_EQ(t->rowCount(), (size_t)2);
    CHECK_EQ(t->schema()[0].name, std::string("x"));   // BOM stripped from header
    CHECK_EQ(t->columnAt(1)[1].toInt64(), (int64_t)4);
}

TEST(csv_tsv_delimiter) {
    CsvOptions o; o.delimiter = '\t';
    TablePtr t = readCsvString("a\tb\n1\t2\n", "t", o);
    CHECK_EQ(t->columnCount(), (size_t)2);
    CHECK_EQ(t->columnAt(1)[0].toInt64(), (int64_t)2);
}

TEST(csv_no_header) {
    CsvOptions o; o.hasHeader = false;
    TablePtr t = readCsvString("1,2\n3,4\n", "t", o);
    CHECK_EQ(t->rowCount(), (size_t)2);
    CHECK_EQ(t->schema()[0].name, std::string("col1"));
}

TEST(csv_import_and_query) {
    // Write a temp CSV, import via Connection, and query it end-to-end.
    std::string path = "._lq_test_import.csv";
    {
        std::ofstream f(path);
        f << "region,amount\nWest,100\nEast,250\nWest,60\n";
    }
    Connection c;
    QueryResult imp = c.importCsv(path, "sales");
    CHECK(imp.ok());
    CHECK_EQ(imp.rowsAffected, (int64_t)3);

    QueryResult r = c.query("SELECT region, SUM(amount) FROM sales GROUP BY region ORDER BY region");
    CHECK(r.ok());
    CHECK_EQ(r.rows.size(), (size_t)2);
    // East=250, West=160
    CHECK_EQ(r.rows[0][0].getString(), std::string("East"));
    CHECK_EQ(r.rows[0][1].toDouble(), 250.0);
    CHECK_EQ(r.rows[1][1].toDouble(), 160.0);

    std::remove(path.c_str());
}

TEST(csv_import_duplicate_table_errors) {
    std::string path = "._lq_test_dup.csv";
    { std::ofstream f(path); f << "a\n1\n"; }
    Connection c;
    CHECK(c.importCsv(path, "t").ok());
    CHECK(!c.importCsv(path, "t").ok());   // second import into same name fails
    std::remove(path.c_str());
}

int main() {
    std::printf("LiteQuery test suite\n====================\n");
    return lqtest::run_all();
}
