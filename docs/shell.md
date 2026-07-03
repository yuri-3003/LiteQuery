# The `lq` shell

`lq` is an interactive SQL shell for LiteQuery — type SQL, see results. It's the
fastest way to explore the engine without writing any C or C++.

## Running

```bash
lq                      # start the interactive REPL
lq script.sql           # run a .sql file, then exit
lq -c "SELECT 1 + 1"    # run one statement, then exit
echo "SELECT 1" | lq    # run SQL piped on stdin, then exit
```

The binary is built by CMake as `lq` (in `build/`). It's also installed by
`cmake --install`.

## A session

```
$ lq
LiteQuery v0.1.0  ·  type .help for commands, .quit to exit
lq> CREATE TABLE emp (id INT, name VARCHAR, dept VARCHAR, salary DOUBLE);
OK
lq> INSERT INTO emp VALUES (1,'Ann','Eng',100),(2,'Bob','Eng',120),(3,'Cy','Sales',90);
OK  (3 rows affected)
lq> SELECT dept, COUNT(*), SUM(salary), AVG(salary) FROM emp GROUP BY dept ORDER BY dept;
┌───────┬───────┬─────┬─────┐
│ dept  │ COUNT │ SUM │ AVG │
├───────┼───────┼─────┼─────┤
│ Eng   │ 2     │ 220 │ 110 │
│ Sales │ 1     │ 90  │ 90  │
└───────┴───────┴─────┴─────┘
2 rows  ·  54 µs
```

Statements are terminated by `;` and may span multiple lines — the prompt
changes to ` ..>` while a statement is still open.

## Dot-commands

| Command | What it does |
|---|---|
| `.help` | list the commands |
| `.tables` | list tables in the database |
| `.schema [table]` | show a CREATE-like column list (all tables, or one) |
| `.import FILE TABLE` | load a CSV file into a new table (types inferred) |
| `.import -t FILE TABLE` | same, but tab-separated (TSV) |
| `.mode MODE` | output format: `table` (default) · `csv` · `json` · `list` |
| `.timing on\|off` | toggle the `N rows · T µs` footer |
| `.read FILE` | execute SQL from a file |
| `.quit` | exit (also `.exit`, `.q`, or Ctrl-D) |

## Output modes

`.mode csv` — RFC-4180-ish CSV with a header row:

```
name,salary
Bob,120
Ann,100
```

`.mode json` — an array of row objects:

```json
[
 {"dept": "Eng", "COUNT": 2},
 {"dept": "Sales", "COUNT": 1}
]
```

`.mode list` — bare `|`-separated values, no header (handy for piping).

## Loading a CSV

```
lq> .import sales.csv sales
OK  (10432 rows into sales)
lq> .schema sales
CREATE TABLE sales (
  region VARCHAR,
  product VARCHAR,
  amount FLOAT64
);
lq> SELECT region, SUM(amount) AS total FROM sales GROUP BY region ORDER BY total DESC;
```

Column names come from the header row; types are inferred per column. Empty
cells load as `NULL`. Use `.import -t file.tsv t` for tab-separated files.

## Scripting

Because `lq` reads dot-commands from files and stdin too, you can script it:

```bash
# generate a CSV report from a query
lq mydata.sql > report.csv     # where mydata.sql sets .mode csv

# one-liner
echo "SELECT COUNT(*) FROM emp;" | lq
```

Colour is used only for an interactive terminal; it's disabled automatically
when output is piped or when `NO_COLOR` is set.
