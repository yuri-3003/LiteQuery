# litequery — Python bindings

Python bindings for [LiteQuery](https://github.com/yuri-3003/LiteQuery), an
embeddable **columnar SQL query engine** — 3–7× faster than SQLite on analytical
aggregations. Pure `ctypes` over LiteQuery's C API, so there's **no Python build
dependency** (no pybind11, no Cython) and it works with any CPython.

```python
import litequery

with litequery.connect() as db:
    db.execute("CREATE TABLE t (id INT, name TEXT, amt DOUBLE)")
    db.execute("INSERT INTO t VALUES (1,'a',10.5),(2,'b',20),(3,'a',5)")

    for row in db.query(
        "SELECT name, SUM(amt) AS total FROM t GROUP BY name ORDER BY name"
    ):
        print(row["name"], row["total"])      # a 15.5 / b 20.0

    # Load a CSV straight into a table (names + types inferred):
    db.import_csv("sales.csv", "sales")
    print(db.query("SELECT COUNT(*) FROM sales").scalar())
```

## Install

The bindings load a compiled LiteQuery shared library. From a source checkout:

```bash
cd bindings/python
python build_lib.py      # builds liblitequery.{dll,so,dylib} into the package
pip install .
```

`build_lib.py` invokes CMake to build a **self-contained** shared library (the
C++ runtime is statically linked, so no toolchain DLLs need to be on your path)
and copies it into the package. Requires CMake and a C++17 compiler.

To point at a library you built elsewhere, set `LITEQUERY_LIBRARY` to its path.

## API

### `litequery.connect() -> Connection`

Opens a new in-memory database. Use it as a context manager (`with
litequery.connect() as db:`) or call `db.close()` yourself.

### `Connection`

| Method | Description |
|---|---|
| `query(sql) -> Result` | Run a SELECT; returns a `Result`. Raises `LiteQueryError` on failure. |
| `execute(sql) -> int` | Run CREATE/INSERT/DROP; returns rows affected. |
| `import_csv(path, table, delimiter=",", has_header=True) -> int` | Load a CSV/TSV into a new table; returns rows loaded. |
| `close()` | Close the connection. |

### `Result`

Iterable of `Row`. Also:

| Member | Description |
|---|---|
| `columns` | list of column names |
| `len(result)` | row count |
| `result[i]` | the i-th `Row` |
| `.rows()` | all rows as `Row` objects |
| `.tuples()` | all rows as plain tuples |
| `.scalar()` | first column of the first row (or `None`) |
| `.to_pandas()` | a `pandas.DataFrame` (requires `pandas`) |
| `.rows_affected`, `.elapsed_micros` | query metadata |

### `Row`

A `dict` keyed by column name that also supports positional access: `row["name"]`
or `row[0]`.

### `LiteQueryError`

Raised for query, parse, and runtime errors.

## Supported SQL

See the [SQL reference](https://github.com/yuri-3003/LiteQuery/blob/master/docs/sql-reference.md):
`SELECT` with `WHERE` / `GROUP BY` / aggregates / `JOIN` / `DISTINCT` /
`ORDER BY` / `LIMIT`, `CREATE` / `DROP` / `INSERT`, and CSV import.

## License

MIT.
