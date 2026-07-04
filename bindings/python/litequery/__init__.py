"""LiteQuery — Python bindings for the embeddable columnar SQL engine.

A thin, dependency-free wrapper over the LiteQuery C API (via ctypes), so it
works with any CPython regardless of how the interpreter was built.

    import litequery

    with litequery.connect() as db:
        db.execute("CREATE TABLE t (id INT, name TEXT, amt DOUBLE)")
        db.execute("INSERT INTO t VALUES (1,'a',10.5),(2,'b',20)")

        for row in db.query("SELECT name, SUM(amt) AS total "
                            "FROM t GROUP BY name ORDER BY name"):
            print(row["name"], row["total"])

        # Load a CSV straight into a table:
        db.import_csv("sales.csv", "sales")
        print(db.query("SELECT COUNT(*) FROM sales").scalar())
"""

from __future__ import annotations

import ctypes
from typing import Any, Iterator, Optional

from . import _ffi
from ._ffi import lib

__all__ = ["connect", "Connection", "Result", "Row", "LiteQueryError", "__version__"]

__version__ = lib.lq_version().decode("utf-8")


class LiteQueryError(Exception):
    """Raised when a query or operation fails."""


class Row(dict):
    """One result row.

    Behaves like a dict keyed by column name, and also supports positional
    access: `row[0]` or `row["name"]`.
    """

    __slots__ = ("_values",)

    def __init__(self, columns: list[str], values: list[Any]):
        super().__init__(zip(columns, values))
        self._values = values

    def __getitem__(self, key: Any) -> Any:
        if isinstance(key, int):
            return self._values[key]
        return super().__getitem__(key)

    @property
    def values_tuple(self) -> tuple:
        return tuple(self._values)


class Result:
    """A materialized query result: column names + rows."""

    def __init__(self, columns: list[str], rows: list[list[Any]],
                 rows_affected: int, elapsed_micros: int):
        self.columns = columns
        self._rows = rows
        self.rows_affected = rows_affected
        self.elapsed_micros = elapsed_micros

    def __len__(self) -> int:
        return len(self._rows)

    def __iter__(self) -> Iterator[Row]:
        for r in self._rows:
            yield Row(self.columns, r)

    def __getitem__(self, i: int) -> Row:
        return Row(self.columns, self._rows[i])

    def rows(self) -> list[Row]:
        """All rows as Row objects."""
        return [Row(self.columns, r) for r in self._rows]

    def tuples(self) -> list[tuple]:
        """All rows as plain tuples (no column names)."""
        return [tuple(r) for r in self._rows]

    def scalar(self) -> Any:
        """The value in the first column of the first row (or None)."""
        if not self._rows:
            return None
        return self._rows[0][0]

    def to_pandas(self):  # pragma: no cover - optional dependency
        """Return the result as a pandas DataFrame (requires pandas)."""
        import pandas as pd
        return pd.DataFrame(self._rows, columns=self.columns)

    def __repr__(self) -> str:
        return f"<litequery.Result {len(self._rows)} rows x {len(self.columns)} cols>"


def _extract_value(res, col: int) -> Any:
    t = lib.lq_result_column_type(res, col)
    if t == _ffi.LQ_TYPE_NULL or lib.lq_result_is_null(res, col):
        return None
    if t == _ffi.LQ_TYPE_INT:
        out = ctypes.c_int64()
        lib.lq_result_get_int(res, col, ctypes.byref(out))
        return out.value
    if t == _ffi.LQ_TYPE_DOUBLE:
        out = ctypes.c_double()
        lib.lq_result_get_double(res, col, ctypes.byref(out))
        return out.value
    if t == _ffi.LQ_TYPE_BOOL:
        out = ctypes.c_int()
        lib.lq_result_get_bool(res, col, ctypes.byref(out))
        return bool(out.value)
    # TEXT (and any fallback)
    raw = lib.lq_result_get_text(res, col)
    return raw.decode("utf-8") if raw is not None else None


class Connection:
    """A LiteQuery database connection (in-memory).

    Not thread-safe; use one connection per thread.
    """

    def __init__(self):
        self._db = lib.lq_open()
        if not self._db:
            raise LiteQueryError("failed to open database")

    # ---- context manager --------------------------------------------------
    def __enter__(self) -> "Connection":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        if getattr(self, "_db", None):
            lib.lq_close(self._db)
            self._db = None

    def __del__(self):  # best-effort cleanup
        try:
            self.close()
        except Exception:
            pass

    # ---- query / execute --------------------------------------------------
    def query(self, sql: str) -> Result:
        """Run a SELECT and return a Result. Raises LiteQueryError on failure."""
        if not self._db:
            raise LiteQueryError("connection is closed")
        res = lib.lq_query(self._db, sql.encode("utf-8"))
        if not res:
            raise LiteQueryError("query returned no result handle")
        try:
            if not lib.lq_result_ok(res):
                msg = lib.lq_result_error(res).decode("utf-8")
                raise LiteQueryError(msg)

            ncol = lib.lq_result_column_count(res)
            columns = [
                lib.lq_result_column_name(res, c).decode("utf-8")
                for c in range(ncol)
            ]
            rows: list[list[Any]] = []
            while lib.lq_result_next(res):
                rows.append([_extract_value(res, c) for c in range(ncol)])

            return Result(
                columns,
                rows,
                lib.lq_result_rows_affected(res),
                lib.lq_result_elapsed_micros(res),
            )
        finally:
            lib.lq_result_free(res)

    def execute(self, sql: str) -> int:
        """Run a statement with no result set (CREATE/INSERT/DROP).

        Returns the number of rows affected. Raises LiteQueryError on failure.
        """
        r = self.query(sql)
        return r.rows_affected

    def import_csv(self, path: str, table_name: str,
                   delimiter: str = ",", has_header: bool = True) -> int:
        """Load a CSV/TSV file into a new table, inferring names and types.

        Returns the number of rows loaded. Raises LiteQueryError on failure.
        """
        if not self._db:
            raise LiteQueryError("connection is closed")
        rows = ctypes.c_int64(0)
        err = ctypes.c_char_p()
        status = lib.lq_import_csv(
            self._db,
            path.encode("utf-8"),
            table_name.encode("utf-8"),
            delimiter.encode("utf-8")[:1] or b",",
            1 if has_header else 0,
            ctypes.byref(rows),
            ctypes.byref(err),
        )
        if status != 0:
            msg = err.value.decode("utf-8") if err.value else "CSV import failed"
            raise LiteQueryError(msg)
        return rows.value

    def save(self, path: str) -> None:
        """Save the whole database (all tables + data) to a file."""
        self._call_pathfn(lib.lq_save, path, "save failed")

    def load(self, path: str) -> None:
        """Load a database from a file (tables are added, replacing same names)."""
        self._call_pathfn(lib.lq_load, path, "load failed")

    def _call_pathfn(self, fn, path: str, default_err: str) -> None:
        if not self._db:
            raise LiteQueryError("connection is closed")
        err = ctypes.c_char_p()
        status = fn(self._db, path.encode("utf-8"), ctypes.byref(err))
        if status != 0:
            msg = err.value.decode("utf-8") if err.value else default_err
            raise LiteQueryError(msg)


def connect() -> Connection:
    """Open a new in-memory LiteQuery database."""
    return Connection()
