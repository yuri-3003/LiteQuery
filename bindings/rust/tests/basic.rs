//! Integration tests for the LiteQuery Rust bindings.

use litequery::{Connection, Value};

#[test]
fn version_is_semver() {
    let v = litequery::version();
    assert!(v.split('.').count() == 3, "version should look like x.y.z: {v}");
}

#[test]
fn create_insert_select() {
    let db = Connection::open().unwrap();
    db.execute("CREATE TABLE t (id INT, name TEXT, amt DOUBLE)").unwrap();
    let n = db
        .execute("INSERT INTO t VALUES (1,'a',10.5),(2,'b',20)")
        .unwrap();
    assert_eq!(n, 2);

    let r = db.query("SELECT id, name, amt FROM t ORDER BY id").unwrap();
    assert_eq!(r.columns, vec!["id", "name", "amt"]);
    assert_eq!(r.len(), 2);

    let rows: Vec<_> = r.rows().collect();
    assert_eq!(rows[0].get_str("name"), Some("a"));
    assert_eq!(rows[0].get_f64("amt"), Some(10.5));
    assert_eq!(rows[1].get_i64("id"), Some(2));
    // positional access
    assert_eq!(rows[1].at(1), Some(&Value::Text("b".into())));
}

#[test]
fn scalar_and_group_by() {
    let db = Connection::open().unwrap();
    db.execute("CREATE TABLE t (g TEXT, v DOUBLE)").unwrap();
    db.execute("INSERT INTO t VALUES ('a',1),('b',2),('a',3)").unwrap();

    let sum = db.query("SELECT SUM(v) FROM t").unwrap();
    assert_eq!(sum.scalar(), Some(&Value::Double(6.0)));

    let r = db
        .query("SELECT g, SUM(v) AS total FROM t GROUP BY g ORDER BY g")
        .unwrap();
    let mut got = std::collections::HashMap::new();
    for row in r.rows() {
        got.insert(row.get_str("g").unwrap().to_string(), row.get_f64("total").unwrap());
    }
    assert_eq!(got["a"], 4.0);
    assert_eq!(got["b"], 2.0);
}

#[test]
fn null_handling() {
    let db = Connection::open().unwrap();
    db.execute("CREATE TABLE t (v DOUBLE)").unwrap();
    db.execute("INSERT INTO t VALUES (10.0), (NULL), (20.0)").unwrap();

    let r = db.query("SELECT v FROM t").unwrap();
    let nulls = r.rows().filter(|row| row.at(0).unwrap().is_null()).count();
    assert_eq!(nulls, 1);

    // AVG skips NULLs: (10+20)/2 = 15
    let avg = db.query("SELECT AVG(v) FROM t").unwrap();
    assert_eq!(avg.scalar(), Some(&Value::Double(15.0)));
}

#[test]
fn errors_are_returned() {
    let db = Connection::open().unwrap();
    assert!(db.query("SELECT * FROM nope").is_err());
    assert!(db.query("SELECT FROM WHERE").is_err()); // syntax error
    // The error carries a message.
    let e = db.query("SELECT * FROM nope").unwrap_err();
    assert!(!e.0.is_empty());
}

#[test]
fn import_csv() {
    use std::io::Write;
    let dir = std::env::temp_dir();
    let path = dir.join("lq_rust_test.csv");
    {
        let mut f = std::fs::File::create(&path).unwrap();
        writeln!(f, "region,amount").unwrap();
        writeln!(f, "West,100").unwrap();
        writeln!(f, "East,250").unwrap();
        writeln!(f, "West,60").unwrap();
    }

    let db = Connection::open().unwrap();
    let n = db.import_csv(path.to_str().unwrap(), "sales").unwrap();
    assert_eq!(n, 3);

    let r = db
        .query("SELECT region, SUM(amount) AS total FROM sales GROUP BY region ORDER BY total DESC")
        .unwrap();
    let rows: Vec<_> = r.rows().collect();
    assert_eq!(rows[0].get_str("region"), Some("East"));
    assert_eq!(rows[0].get_f64("total"), Some(250.0));
    assert_eq!(rows[1].get_f64("total"), Some(160.0));

    let _ = std::fs::remove_file(&path);
}

#[test]
fn import_missing_file_errors() {
    let db = Connection::open().unwrap();
    assert!(db.import_csv("/no/such/file.csv", "t").is_err());
}

#[test]
fn save_and_load() {
    let path = std::env::temp_dir().join("lq_rust_persist.lqdb");
    let path_str = path.to_str().unwrap();
    {
        let db = Connection::open().unwrap();
        db.execute("CREATE TABLE t (id INT, name TEXT, v DOUBLE)").unwrap();
        db.execute("INSERT INTO t VALUES (1,'a',1.5),(2,'b',NULL),(3,'c',3.5)").unwrap();
        db.save(path_str).unwrap();
    }
    {
        let db = Connection::open().unwrap(); // fresh, empty
        db.load(path_str).unwrap();
        let r = db.query("SELECT id, name, v FROM t ORDER BY id").unwrap();
        let rows: Vec<_> = r.rows().collect();
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].get_str("name"), Some("a"));
        assert!(rows[1].at(2).unwrap().is_null()); // NULL preserved
        // SUM skips the NULL: 1.5 + 3.5 = 5.0
        assert_eq!(db.query("SELECT SUM(v) FROM t").unwrap().scalar().unwrap().as_f64(), Some(5.0));
    }
    let _ = std::fs::remove_file(&path);
}

#[test]
fn load_bad_file_errors() {
    let db = Connection::open().unwrap();
    assert!(db.load("/no/such/db.lqdb").is_err());
}
