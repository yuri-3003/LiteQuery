"""Tests for the LiteQuery Python bindings (stdlib unittest — no deps).

Run from bindings/python with the shared library built into the package:

    python -m unittest discover -s tests -v
"""

import os
import tempfile
import unittest

import litequery


class TestBasics(unittest.TestCase):
    def setUp(self):
        self.db = litequery.connect()

    def tearDown(self):
        self.db.close()

    def test_version(self):
        self.assertRegex(litequery.__version__, r"^\d+\.\d+\.\d+$")

    def test_create_insert_select(self):
        self.db.execute("CREATE TABLE t (id INT, name TEXT, amt DOUBLE)")
        n = self.db.execute("INSERT INTO t VALUES (1,'a',10.5),(2,'b',20)")
        self.assertEqual(n, 2)
        r = self.db.query("SELECT id, name, amt FROM t ORDER BY id")
        self.assertEqual(r.columns, ["id", "name", "amt"])
        self.assertEqual(len(r), 2)
        self.assertEqual(r[0]["name"], "a")
        self.assertEqual(r[0]["amt"], 10.5)
        self.assertEqual(r[1][0], 2)  # positional access

    def test_scalar_and_tuples(self):
        self.db.execute("CREATE TABLE t (x INT)")
        self.db.execute("INSERT INTO t VALUES (10),(20),(30)")
        self.assertEqual(self.db.query("SELECT SUM(x) FROM t").scalar(), 60)
        self.assertEqual(
            self.db.query("SELECT x FROM t ORDER BY x").tuples(),
            [(10,), (20,), (30,)],
        )

    def test_group_by(self):
        self.db.execute("CREATE TABLE t (g TEXT, v DOUBLE)")
        self.db.execute("INSERT INTO t VALUES ('a',1),('b',2),('a',3)")
        r = self.db.query(
            "SELECT g, SUM(v) AS total FROM t GROUP BY g ORDER BY g"
        )
        rows = {row["g"]: row["total"] for row in r}
        self.assertEqual(rows, {"a": 4.0, "b": 2.0})

    def test_null_values(self):
        self.db.execute("CREATE TABLE t (v DOUBLE)")
        self.db.execute("INSERT INTO t VALUES (10.0), (NULL), (20.0)")
        r = self.db.query("SELECT v FROM t")
        vals = [row[0] for row in r]
        self.assertIn(None, vals)
        # AVG skips NULLs: (10+20)/2 = 15
        self.assertEqual(self.db.query("SELECT AVG(v) FROM t").scalar(), 15.0)

    def test_error_raises(self):
        with self.assertRaises(litequery.LiteQueryError):
            self.db.query("SELECT * FROM does_not_exist")
        with self.assertRaises(litequery.LiteQueryError):
            self.db.query("SELECT FROM WHERE")  # syntax error

    def test_context_manager(self):
        with litequery.connect() as db:
            db.execute("CREATE TABLE t (x INT)")
            db.execute("INSERT INTO t VALUES (1),(2)")
            self.assertEqual(db.query("SELECT COUNT(*) FROM t").scalar(), 2)


class TestCsvImport(unittest.TestCase):
    def test_import_csv(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "sales.csv")
            with open(path, "w", newline="") as f:
                f.write("region,amount\nWest,100\nEast,250\nWest,60\n")
            with litequery.connect() as db:
                n = db.import_csv(path, "sales")
                self.assertEqual(n, 3)
                r = db.query(
                    "SELECT region, SUM(amount) AS total "
                    "FROM sales GROUP BY region ORDER BY total DESC"
                )
                self.assertEqual(r[0]["region"], "East")
                self.assertEqual(r[0]["total"], 250.0)
                self.assertEqual(r[1]["total"], 160.0)

    def test_import_missing_file_raises(self):
        with litequery.connect() as db:
            with self.assertRaises(litequery.LiteQueryError):
                db.import_csv("/no/such/file.csv", "t")


if __name__ == "__main__":
    unittest.main()
