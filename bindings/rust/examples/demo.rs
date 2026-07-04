//! A small tour of the LiteQuery Rust API.
//!
//!     cargo run --example demo

use litequery::Connection;

fn main() -> Result<(), litequery::Error> {
    println!("LiteQuery {} (Rust bindings)\n", litequery::version());

    let db = Connection::open()?;
    db.execute("CREATE TABLE sales (region TEXT, product TEXT, amount DOUBLE)")?;
    db.execute(
        "INSERT INTO sales VALUES \
         ('West','Widget',100),('West','Gadget',250),\
         ('East','Widget',120),('East','Gadget',80),('West','Widget',60)",
    )?;

    println!("Revenue by region:");
    let r = db.query(
        "SELECT region, COUNT(*) AS n, SUM(amount) AS total \
         FROM sales GROUP BY region ORDER BY total DESC",
    )?;
    for row in r.rows() {
        println!(
            "  {:<5}  {} orders  ${}",
            row.get_str("region").unwrap_or(""),
            row.get_i64("n").unwrap_or(0),
            row.get_f64("total").unwrap_or(0.0),
        );
    }

    let top = db
        .query("SELECT product, SUM(amount) AS total FROM sales GROUP BY product ORDER BY total DESC LIMIT 1")?;
    if let Some(row) = top.rows().next() {
        println!(
            "\nTop product: {} (${})",
            row.get_str("product").unwrap_or(""),
            row.get_f64("total").unwrap_or(0.0),
        );
    }

    Ok(())
}
