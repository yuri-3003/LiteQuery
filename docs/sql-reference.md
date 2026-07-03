# LiteQuery SQL Reference

The exact SQL subset LiteQuery supports today. Anything not listed here is not
implemented (the parser may accept some of it but execution will report an
error). Keywords are case-insensitive; identifiers are case-insensitive for
lookup.

## Loading data from CSV

Besides `INSERT`, tables can be created directly from a CSV/TSV file. This is
exposed through the API and the `lq` shell rather than SQL syntax:

- **C++:** `connection.importCsv("sales.csv", "sales")` — see
  [c-api.md](c-api.md) / the `Connection` header.
- **Shell:** `.import sales.csv sales` (or `.import -t data.tsv t` for TSV).

Column **names** come from the header row; column **types** are inferred per
column (a column is `BIGINT` if every value is an integer, else `DOUBLE` if
every value is numeric, else `VARCHAR`). Empty fields become `NULL`. Quoting
(`"a,b"`, `""` escapes), embedded newlines in quotes, CRLF, and a leading UTF-8
BOM are all handled.

## Statements

### CREATE TABLE

```sql
CREATE TABLE <name> (
    <col> <type> [NOT NULL] [PRIMARY KEY] [DEFAULT <expr>],
    ...
);
```

Constraints are parsed; `NOT NULL` is recorded on the column type. `PRIMARY KEY`,
`DEFAULT`, and `CHECK` are parsed but not yet enforced. `IF NOT EXISTS` is **not**
supported.

### DROP TABLE

```sql
DROP TABLE <name>;
```

`IF EXISTS` is not supported; dropping a missing table is an error.

### INSERT

```sql
INSERT INTO <name> [(col, ...)] VALUES (v, ...), (v, ...), ...;
```

Values are scalar expressions evaluated at insert time (constants and constant
expressions like `1 + 2`). `INSERT … SELECT` is **not** implemented.

### SELECT

```sql
SELECT [DISTINCT] <select_list>
[FROM <table_ref> [, <table_ref> ...]]
[WHERE <predicate>]
[GROUP BY <expr> [, <expr> ...]]
[HAVING <predicate>]
[ORDER BY <expr> [ASC|DESC] [NULLS FIRST|LAST] [, ...]]
[LIMIT <n> [OFFSET <m>]]
[UNION [ALL] <select>]
```

- `<select_list>` is `*`, `table.*`, or a comma-separated list of
  `<expr> [[AS] alias]`.
- A missing `FROM` evaluates a single row (`SELECT 1 + 1`).
- `ORDER BY` may reference columns not in the select list.
- With `GROUP BY`, `ORDER BY` may reference a **group-key column** or an
  **aliased aggregate** (`SELECT SUM(x) AS total … ORDER BY total`). Ordering by
  a *bare* aggregate expression (`ORDER BY SUM(x)` with no alias) is not yet
  supported — give the aggregate an alias and order by that.
- `UNION` is modeled by the planner; execution of set operations is partial.

## Table references & joins

```sql
FROM t                          -- base table
FROM t AS x                     -- aliased (columns become x.col)
FROM (SELECT ...) AS sub        -- subquery
FROM a JOIN b ON a.k = b.k      -- INNER (bare JOIN == INNER JOIN)
FROM a LEFT  [OUTER] JOIN b ON ...
FROM a RIGHT [OUTER] JOIN b ON ...
FROM a FULL  [OUTER] JOIN b ON ...
FROM a CROSS JOIN b             -- no ON clause
FROM a, b                       -- comma == CROSS JOIN
```

Qualify columns as `alias.col` when a table is aliased. `ON` predicates may be
any boolean expression (equi-join and non-equi conditions both work; the current
join operator evaluates the predicate per candidate pair).

## Expressions

### Literals

`123` (INT64), `3.14` (DOUBLE), `'text'` (VARCHAR, `''` escapes a quote),
`TRUE`, `FALSE`, `NULL`.

### Operators (precedence low → high)

| Level | Operators |
|---|---|
| 1 | `OR` |
| 2 | `AND` |
| 3 | `NOT` (prefix) |
| 4 | `=` `<>` `!=` `<` `<=` `>` `>=` `LIKE` `ILIKE`; postfix `IS [NOT] NULL`, `[NOT] BETWEEN`, `[NOT] IN` |
| 5 | `||` (string concat) |
| 6 | `+` `-` |
| 7 | `*` `/` `%` |
| 8 | `^` (power, right-assoc) |
| 9 | unary `+` `-` |

### Predicates

```sql
x IS NULL              x IS NOT NULL
x BETWEEN lo AND hi    x NOT BETWEEN lo AND hi
x IN (a, b, c)         x NOT IN (a, b, c)
s LIKE 'a%b_'          s ILIKE 'A%'          -- % = any run, _ = one char
```

`IN (subquery)` is parsed but not executed.

### CASE

```sql
CASE WHEN cond THEN r [WHEN ...] [ELSE r] END      -- searched
CASE subject WHEN v THEN r [WHEN ...] [ELSE r] END -- simple
```

### CAST

```sql
CAST(expr AS <type>)
```

Supported target types: integer types, `FLOAT`/`DOUBLE`/`REAL`, `VARCHAR`/`TEXT`,
`BOOLEAN`. (The `::` shorthand is tokenized but not wired into the parser.)

### Scalar functions

| Function | Result |
|---|---|
| `COALESCE(a, b, …)` | first non-NULL argument |
| `ABS(x)` | absolute value |
| `LENGTH(s)` | string length |
| `UPPER(s)` / `LOWER(s)` | case conversion |
| `ROUND(x [, digits])` | rounding |

## Aggregate functions

Usable with or without `GROUP BY`:

| Function | Notes |
|---|---|
| `COUNT(*)` | counts rows |
| `COUNT(expr)` | counts non-NULL values |
| `COUNT(DISTINCT expr)` | counts distinct non-NULL values |
| `SUM(expr)` | NULL if no non-NULL inputs |
| `AVG(expr)` | mean of non-NULL inputs |
| `MIN(expr)` / `MAX(expr)` | extrema (typed) |

Aggregates ignore `NULL` inputs. An aggregate query with no rows still returns
one row (`COUNT` → 0, others → NULL). `HAVING` is parsed and applied as a filter
over aggregate output; complex `HAVING` expressions are partially supported.

## Data types

| SQL | TypeId | Storage |
|---|---|---|
| `BOOLEAN`, `BOOL` | BOOLEAN | 1 byte |
| `TINYINT` | INT8 | 1 byte |
| `SMALLINT` | INT16 | 2 bytes |
| `INT`, `INTEGER` | INT32 | 4 bytes |
| `BIGINT` | INT64 | 8 bytes |
| `REAL`, `FLOAT` | FLOAT32 | 4 bytes |
| `DOUBLE` | FLOAT64 | 8 bytes |
| `DECIMAL(p,s)`, `NUMERIC(p,s)` | DECIMAL | int64 mantissa + scale |
| `VARCHAR(n)`, `TEXT` | VARCHAR | UTF-8 string |
| `DATE` | DATE | days since epoch |
| `TIMESTAMP` | TIMESTAMP | microseconds since epoch |
| `BLOB` | BLOB | raw bytes |

Integer literals are `INT64` and float literals are `FLOAT64`; they promote to a
column's declared type on insert.

## NULL semantics

LiteQuery follows standard SQL three-valued logic:

- Any arithmetic or comparison involving `NULL` yields `NULL`.
- `WHERE`/`HAVING`/`ON` treat a `NULL` result as **not true** (row excluded).
- `false AND NULL → false`; `true OR NULL → true`; otherwise `NULL` propagates.
- Aggregates skip `NULL` inputs; `COUNT(*)` counts all rows regardless.
