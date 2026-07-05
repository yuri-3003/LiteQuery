# LiteQuery Architecture

How a SQL string becomes a result set, component by component.

## Overview

LiteQuery is a pull-based, columnar query engine. Data is stored one column at a
time; execution is a tree of operators where each parent pulls a batch of rows
from its child (the "Volcano" model, batched).

```
SQL text -> Lexer -> Parser -> AST -> operator tree -> QueryResult
```

The `Connection` builds a physical operator tree directly from the AST, resolved
against the catalog. There is no separate logical-plan representation to keep in
sync; `EXPLAIN` prints the same tree the executor runs.

## Directory layout

```
include/litequery/litequery.h   Public C API (the only header consumers need)
src/
  parser/
    lexer.{h,cpp}       SQL text -> vector<Token>
    ast.h               AST node types (std::variant + visitor)
    parser.{h,cpp}      tokens -> ast::StmtNode  (recursive descent + Pratt)
  catalog/
    catalog.h           Table registry (name -> schema + data), thread-safe
  storage/
    column.h            Typed column + validity bitmap
    table.h             Table = named collection of columns
    csv_reader.{h,cpp}  CSV/TSV ingestion with type inference
    persistence.{h,cpp} Save/load a database to a file
  execution/
    eval.{h,cpp}            Scalar expression evaluator
    physical_plan.{h,cpp}   Pull-model operators (scan/filter/agg/join/...)
    fast_aggregate.{h,cpp}  Typed vectorized path for common aggregates
  api/
    connection.{h,cpp}  SQL -> operator tree -> QueryResult
    c_api.cpp           extern "C" wrapper over Connection
tests/                  Test framework + integration tests + a pure-C test
tools/lq_shell.cpp      The lq interactive shell
examples/demo.cpp       CLI tour
bench/                  Benchmarks (incl. an opt-in LiteQuery-vs-SQLite harness)
bindings/{python,rust}  Language bindings over the C API
```

## Type system (`include/litequery/types.h`)

- `TypeId` — the type discriminator (`INT32`, `FLOAT64`, `VARCHAR`, ...).
- `DataType` — a `TypeId` plus modifiers (nullability, VARCHAR length, DECIMAL
  precision/scale).
- `Value` — one scalar held in a `std::variant` (or `NullValue`). Implements SQL
  comparison semantics (any comparison with `NULL` is not true) and
  `toInt64()`/`toDouble()` for type-agnostic arithmetic.
- `Schema` — an ordered list of `(name, DataType)`. Column lookup (`indexOf`) is
  case-insensitive and matches both bare (`col`) and qualified (`alias.col`) names.
- `Batch` — a columnar slice of up to 1024 rows passed between operators.

## Lexer (`parser/lexer.*`)

`Lexer::tokenize()` scans the source in one pass into a flat `vector<Token>`
terminated by `END_OF_FILE`. `Token::lexeme` is a `std::string_view` into the
source buffer, so the source string must outlive the tokens. Numeric and string
values are decoded lazily via `Token::intValue()` / `stringValue()`. Lexical
errors are collected rather than thrown, so one bad query yields all its errors
at once.

## Parser (`parser/parser.*`)

A hand-written recursive-descent parser for statements and a Pratt
(precedence-climbing) parser for expressions. Precedence, low to high:
`OR < AND < NOT < comparisons/IS/IN/LIKE/BETWEEN < + - < * / % < ^ < unary`.

Postfix constructs (`IS NULL`, `BETWEEN ... AND ...`, `IN (...)`, `[NOT] LIKE`)
are handled in `parsePostfix`. The parser copies each identifier and string out
of the token's `string_view` into an owned `std::string`, so the resulting AST
is self-contained and outlives the token buffer. Errors throw `ParseError` with
a `SourceLocation`.

## AST (`parser/ast.h`)

Nodes are plain structs; the union of all expression node types is
`ExprNode = std::variant<Literal, ColumnRef, BinaryExpr, ...>` and children are
held by `unique_ptr` (`using Expr = unique_ptr<ExprNode>`). Dispatch is via
`std::visit`, giving compile-time exhaustiveness without virtual calls.

## Storage (`storage/column.h`, `storage/table.h`, `catalog/catalog.h`)

A `Table` owns one `Column` per schema field. A `Column` stores its values in a
typed contiguous buffer — `vector<int64_t>`, `vector<double>`, or
`vector<string>` chosen by the column's `TypeId` — plus a `ValidityBitmap`
(1 bit/row, 1 = non-null). Narrow integers are widened to int64 in memory; the
declared `DataType` is kept for the schema.

The column exposes both a boxed API (`operator[]` returning `Value`) for
row-oriented operators and typed accessors (`i64()`, `f64()`, `str()`,
`validity()`) for the hot paths. `Catalog` maps table names to
`shared_ptr<Table>` and guards access with a `shared_mutex`.

## Expression evaluator (`execution/eval.*`)

`evaluate(expr, schema, row)` returns a `Value`. It implements:

- Arithmetic with numeric promotion (integer ops stay integer; mixing with a
  float promotes to double). Division and modulo by zero yield `NULL`.
- Three-valued logic: `false AND x` is `false`, `true OR x` is `true`,
  otherwise `NULL` propagates. `WHERE` treats a `NULL` predicate as false.
- Comparisons across compatible numeric types, strings, and booleans.
- `LIKE`/`ILIKE` with `%` (any run) and `_` (one char).
- `CASE` (simple and searched), `CAST`, `IS [NOT] NULL`, `BETWEEN`, `IN (list)`,
  and a small set of scalar functions.

Aggregate calls are not evaluated here; the aggregate operator owns them, and a
stray aggregate reaching the evaluator is an error.

## Physical operators (`execution/physical_plan.*`)

Every operator implements `Operator`:

```cpp
virtual const Schema& schema() const;       // what this operator produces
virtual Batch next();                        // one batch; empty batch = EOS
virtual void  reset();                       // rewind (for re-scans)
virtual std::string explain(int indent) const;  // one line + children (EXPLAIN)
```

Operators are pulled in a loop from the root; `drain(root)` collects the whole
stream into one `Batch`.

| Operator | Kind | Notes |
|---|---|---|
| `SeqScan` | streaming | reads a `Table` column by column into batches |
| `Filter` | streaming | keeps rows where the predicate is true |
| `Project` | streaming | evaluates output expressions per row |
| `Limit` | streaming | applies `OFFSET` then `LIMIT` |
| `Sort` | blocking | `std::stable_sort` with NULL ordering |
| `Distinct` | blocking | dedup via a NULL-aware row key |
| `HashAggregate` | blocking | hashes group keys; SUM/COUNT/AVG/MIN/MAX |
| `HashJoin` | blocking | INNER/LEFT/RIGHT/FULL/CROSS via the ON predicate |
| `Append` | streaming | concatenates two inputs (UNION ALL) |
| `Values` | streaming | constant rows (and the no-FROM single row) |

Blocking operators consume their entire input before producing output; streaming
operators transform one batch at a time.

## Typed fast path (`execution/fast_aggregate.*`)

`tryFastAggregate()` recognizes the common analytical shape — `SELECT [key,]
AGG(col)... FROM <single table> [WHERE conjunction of col<op>const] [GROUP BY
key]` — and runs it directly over the typed column arrays: no `Batch`, no
boxing, no per-row `std::variant` dispatch. It is tried first in
`Connection::runSelect`; when the query does not match (joins, subqueries,
complex predicates, `HAVING`, `COUNT(DISTINCT)`, ...) it returns `nullopt` and
the general operator tree runs instead. Both paths are covered by the same
tests. This path is several times faster than the general path (see `bench/`).

## Connection: SQL to results (`api/connection.*`)

`Connection::query` runs the pipeline and never throws; it catches exceptions
and returns them in `QueryResult::error`. Statement dispatch:

- `CREATE TABLE` — build a `Schema`, register an empty `Table`.
- `DROP TABLE` — remove from the catalog.
- `INSERT` — evaluate values (or run the source `SELECT`), append rows.
- `UPDATE` / `DELETE` — evaluate the WHERE/SET expressions per row and rebuild
  the table.
- `SELECT` — `buildSelect` constructs the operator tree, then `drain`.

`buildSelect` wires operators in SQL evaluation order. Non-aggregate `ORDER BY`
runs before projection, because it may reference columns the projection drops
(`SELECT name ... ORDER BY salary`); `Project` and `Distinct` preserve row
order, so the ordering survives. For aggregate queries, expressions built around
aggregates (in `SELECT`, `HAVING`, `ORDER BY`) are rewritten so each aggregate
call becomes a reference to that aggregate's output column, computed once.
Schema resolution qualifies aliased columns as `alias.col` so join `ON`
predicates resolve; unaliased base tables keep bare column names.

## C API (`api/c_api.cpp`)

Opaque handles (`lq_db`, `lq_result`) wrap the C++ objects. Every `extern "C"`
entry point is exception-guarded, so no C++ exception crosses into C: a thrown
error becomes an error stored on the handle or a status code. A result handle
owns the materialized `QueryResult` plus an iteration cursor and a scratch
string for `lq_result_get_text`. See [c-api.md](c-api.md).

## Testing

`tests/test_framework.h` is a small zero-dependency harness (`TEST`, `CHECK`,
`CHECK_EQ`) that self-registers cases. `tests/test_litequery.cpp` covers the
lexer, parser, evaluator, and the full SQL pipeline; `tests/test_capi.c` is a
pure-C program that exercises the engine through only the public header — its
compilation is itself a test of embeddability. Both are wired into CTest.

## Where to make a change

- Add SQL syntax: `parser.cpp` (and an AST node if needed).
- Add an expression or scalar function: `eval.cpp`.
- Add a clause or operator: a new operator in `physical_plan.*` and its wiring
  in `connection.cpp` (`buildSelect`).
- Speed up a query shape: extend `fast_aggregate.cpp`.
