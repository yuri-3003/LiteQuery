# LiteQuery Architecture

This document explains how a SQL string becomes a result set, component by
component. It is the map you want before changing engine internals.

## The big picture

LiteQuery is a **pull-based, columnar** query engine. Data is stored one column
at a time; execution is a tree of operators where each parent repeatedly *pulls*
a batch of rows from its child (the classic "Volcano" model, batched).

```
 SQL text ─▶ Lexer ─▶ Parser ─▶ AST ─▶ (Logical plan ─▶ Optimizer)   ← EXPLAIN
                                    │
                                    └─▶ Operator tree ─▶ QueryResult  ← execution
```

Two things are worth stating up front because they explain the shape of the
code:

1. **There are two consumers of the AST.** The `LogicalPlanner` + `Optimizer`
   build and rewrite a relational-algebra tree used for `EXPLAIN` and as the
   home of the optimization rules. Separately, the `Connection` builds a
   *physical operator tree directly from the AST* (resolved against the
   catalog) to produce results. Unifying these is the top structural item on the
   roadmap.
2. **The MVP boxes values.** A column is a `std::vector<lq::Value>` (a
   `std::variant`). This is correct and simple; the performance milestone
   replaces it with typed buffers without changing operator interfaces.

## Directory layout

```
include/litequery/litequery.h   Public C API (the only header consumers need)
src/
  parser/
    lexer.{h,cpp}               SQL text → vector<Token>
    ast.h                       AST node types (std::variant + visitor)
    parser.{h,cpp}              tokens → ast::StmtNode  (recursive descent + Pratt)
  planner/
    logical_plan.{h,cpp}        AST → relational algebra tree; PlanPrinter (EXPLAIN)
    optimizer.{h,cpp}           Rule-based fixed-point optimizer
  catalog/
    catalog.h                   Table registry (name → schema + data), thread-safe
  storage/
    table.h                     Columnar Table + Column
  execution/
    eval.{h,cpp}                Scalar expression evaluator (SQL semantics)
    physical_plan.{h,cpp}       Pull-model operators (scan/filter/agg/join/…)
  api/
    connection.{h,cpp}          SQL → operator tree → QueryResult
    c_api.cpp                   extern "C" wrapper over Connection
tests/                          Test framework + unit/integration + pure-C test
examples/demo.cpp               CLI tour
```

## The type system (`types.h`)

Everything rests on a handful of value types:

- **`TypeId`** — the discriminator (`INT32`, `FLOAT64`, `VARCHAR`, …).
- **`DataType`** — a `TypeId` plus modifiers (nullability, VARCHAR length,
  DECIMAL precision/scale).
- **`Value`** — a single scalar held in a `std::variant` (or `NullValue`). It
  implements SQL comparison semantics (any comparison with `NULL` is *not true*)
  and `toInt64()`/`toDouble()` for type-agnostic arithmetic.
- **`Schema`** — an ordered list of `(name, DataType)`. Column lookup
  (`indexOf`) is case-insensitive and matches both bare (`col`) and qualified
  (`alias.col`) names.
- **`Batch`** — a columnar slice of up to 1024 rows passed between operators.

## Lexer (`parser/lexer.*`)

`Lexer::tokenize()` scans the source in one pass into a flat `vector<Token>`
terminated by `END_OF_FILE`. `Token::lexeme` is a `std::string_view` **into the
source buffer**, so the source string must outlive the tokens. Numeric/string
values are decoded lazily via `Token::intValue()` / `stringValue()`. Lexical
errors are collected (not thrown) so one bad query yields all its errors at once.

## Parser (`parser/parser.*`)

A hand-written recursive-descent parser for statements and a **Pratt
(precedence-climbing) parser** for expressions. Precedence, low to high:
`OR < AND < NOT < comparisons/IS/IN/LIKE/BETWEEN < + - < * / % < ^ < unary`.

Postfix constructs (`IS NULL`, `BETWEEN … AND …`, `IN (…)`, `[NOT] LIKE`) are
handled in `parsePostfix`. The parser copies every identifier and string out of
the token's `string_view` into an owned `std::string`, so the resulting AST is
self-contained and outlives the token buffer.

Errors throw `ParseError` with a `SourceLocation`.

## AST (`parser/ast.h`)

Nodes are plain structs; the union of all expression node types is
`ExprNode = std::variant<Literal, ColumnRef, BinaryExpr, …>` and children are
held by `unique_ptr` (`using Expr = unique_ptr<ExprNode>`). Dispatch is via
`std::visit`, giving compile-time exhaustiveness without virtual calls. A CRTP
`ASTVisitor` and an `ASTDebugPrinter` are provided.

## Logical plan & optimizer (`planner/*`)

`LogicalPlanner` turns a `SelectStmt` into a tree of relational operators
(`LogicalScan`, `LogicalFilter`, `LogicalProject`, `LogicalAggregate`,
`LogicalJoin`, …), each carrying its `outputSchema` and a `nodeId`.

`Optimizer` applies `Rule`s to a **fixed point** (repeat until no rule fires):

| Rule | What it does |
|---|---|
| `ConstantFolding`   | Fold constant sub-expressions to literals |
| `PredicateSimplify` | Algebraic simplification of boolean predicates |
| `OuterJoinSimplify` | Demote outer→inner join when a predicate rejects nulls |
| `PredicatePushdown` | Push filters toward scans (through project/join/agg/union) |
| `ColumnPruning`     | Drop columns no ancestor needs (top-down, once per pass) |
| `ProjectMerge`      | Collapse adjacent projections |
| `LimitPushdown`     | Mark sorts feeding a limit for top-K execution |

The driver (`Optimizer::optimize`) walks the tree either bottom-up or top-down
per rule and tracks per-rule statistics (`OptimizerStats`).

`PlanPrinter::print` renders the (optimized) plan as the indented tree you see
from `Connection::explain`.

## Storage (`storage/column.h`, `storage/table.h`, `catalog/catalog.h`)

A `Table` owns one `Column` per schema field. A `Column` (`storage/column.h`)
stores its values in a **typed contiguous buffer** — one of `vector<int64_t>`,
`vector<double>`, or `vector<string>` chosen by the column's `TypeId` — plus an
Arrow-style `ValidityBitmap` (1 bit/row, 1 = non-null). Narrow integers are
widened to int64 in memory; the declared `DataType` is kept for the schema.

The column exposes both a boxed API (`operator[] → Value`, `append(Value)`) for
row-oriented operators and tests, and typed accessors (`i64()`, `f64()`,
`str()`, `validity()`) for the hot paths. `Catalog` maps table names to
`shared_ptr<Table>`, guards access with a `shared_mutex`, and hands out plan
node ids (`allocNodeId`).

## Typed fast aggregate (`execution/fast_aggregate.*`)

`tryFastAggregate()` recognizes the common analytical shape — `SELECT [key,]
AGG(col)… FROM <single table> [WHERE conjunction of col<op>const] [GROUP BY
key]` — and executes it directly over the typed column arrays: no `Batch`, no
boxing, no per-row `std::variant` dispatch. It's tried first in
`Connection::runSelect`; when the query doesn't match (joins, subqueries,
complex predicates, `HAVING`, `COUNT(DISTINCT)`, …) it returns `nullopt` and the
general operator tree runs instead. Both paths are covered by the same tests, so
they can't silently diverge. This is 3–26× faster than the boxed path (`bench/`).

## Expression evaluator (`execution/eval.*`)

`evaluate(expr, schema, row) → Value` is the semantic heart. It implements:

- **Arithmetic** with numeric promotion (integer ops stay integer; mixing with a
  float promotes to double). Division / modulo by zero yields `NULL`.
- **Three-valued logic**: `false AND x → false`, `true OR x → true`, otherwise
  `NULL` propagates. `WHERE` treats a `NULL` predicate as false.
- **Comparisons** across compatible numeric types, strings, and booleans.
- **`LIKE`/`ILIKE`** with `%` (any run) and `_` (one char) via backtracking.
- **`CASE`** (simple and searched), **`CAST`**, **`IS [NOT] NULL`**,
  **`BETWEEN`**, **`IN (list)`**, and a small set of scalar functions.

Aggregate function calls are *not* evaluated here — the aggregate operator owns
them; a stray aggregate reaching the evaluator is an error.

## Physical operators (`execution/physical_plan.*`)

Every operator implements `Operator`:

```cpp
virtual const Schema& schema() const;  // what this operator produces
virtual Batch next();                  // one batch; empty batch = end of stream
virtual void  reset();                 // rewind (for re-scans)
```

Operators are pulled in a loop from the root; `drain(root)` collects the whole
stream into one `Batch`.

| Operator | Kind | Notes |
|---|---|---|
| `SeqScan` | streaming | reads a `Table` column by column into batches |
| `Filter` | streaming | keeps rows where the predicate evaluates true |
| `Project` | streaming | evaluates output expressions per row |
| `Limit` | streaming | applies `OFFSET` then `LIMIT` |
| `Sort` | blocking | materializes, `std::stable_sort` with NULL ordering |
| `Distinct` | blocking | dedup via a NULL-aware row key |
| `HashAggregate` | blocking | hashes group keys; accumulates SUM/COUNT/AVG/MIN/MAX |
| `HashJoin` | blocking | materializes both sides; INNER/LEFT/RIGHT/FULL/CROSS via ON predicate |
| `Values` | streaming | constant rows (and the no-FROM "dual" row) |

*Blocking* operators consume their entire input before producing output;
*streaming* operators transform one batch at a time.

## Connection: from SQL to results (`api/connection.*`)

`Connection::query` runs the pipeline and **never throws** — it catches every
exception and returns it in `QueryResult::error`. Statement dispatch:

- **`CREATE TABLE`** → build a `Schema`, register an empty `Table`.
- **`DROP TABLE`** → remove from the catalog.
- **`INSERT … VALUES`** → evaluate each value expression, append rows.
- **`SELECT`** → `buildSelect` constructs the operator tree, then `drain`.

`buildSelect` wires operators in SQL evaluation order, with one deliberate
choice: **`ORDER BY` runs before projection**, because `ORDER BY` may reference
columns the projection drops (`SELECT name … ORDER BY salary`). `Project` and
`Distinct` preserve row order, so the ordering established by `Sort` survives to
the output. Schema resolution qualifies aliased columns as `alias.col` so join
`ON` predicates resolve; unaliased base tables keep bare column names.

## C API (`api/c_api.cpp`)

Opaque handles (`lq_db`, `lq_result`) wrap the C++ objects. Every `extern "C"`
entry point is exception-guarded so **no C++ exception ever crosses into C** — a
thrown error becomes an error stored on the handle or a status code. A result
handle owns the materialized `QueryResult` plus an iteration cursor and a scratch
string for `lq_result_get_text`. See [c-api.md](c-api.md).

## Testing

`tests/test_framework.h` is a ~90-line zero-dependency harness (`TEST`, `CHECK`,
`CHECK_EQ`) that self-registers cases. `tests/test_litequery.cpp` covers the
lexer, parser, evaluator, and the full SQL pipeline; `tests/test_capi.c` is a
**pure-C** program that exercises the engine through only the public header —
its compilation is itself a test of embeddability. Both are wired into CTest
(`unit`, `capi`).

## Where to start if you want to…

- **Add a SQL feature** → parser (`parser.cpp`) to build the AST node, then
  `eval.cpp` (expression) or `connection.cpp` / a new operator (clause).
- **Add an optimization** → derive from `Rule` in `optimizer.h`, implement
  `apply`, register it in `Optimizer::addDefaultRules`.
- **Make it faster** → the roadmap's #1: typed columnar buffers in
  `storage/table.h` + typed operator paths in `physical_plan.cpp`.
