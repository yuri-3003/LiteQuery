#pragma once

// ============================================================================
// LiteQuery — table.h
// Column-oriented in-memory table storage.
//
// A Table owns one typed Column (see column.h) per schema field. Each Column
// stores its values in a contiguous typed buffer (int64/double/string) plus a
// 1-bit-per-row validity bitmap — the columnar performance foundation. Scans
// and aggregations read a flat array instead of chasing boxed variants.
//
// The Table API (columnAt/rowCount/insertRow/bulkInsertColumns/getRow) is
// unchanged from the earlier boxed form, so the executor and tests are
// untouched; hot paths reach through columnAt(i) to the typed accessors.
//
// Concurrency: a Table is not internally synchronized. The Catalog guards
// table lookup; callers serialize writes to a given table.
// ============================================================================

#include "column.h"
#include "types.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace lq {

// ============================================================================
// Table — a named, schema-typed collection of columns (all the same length)
// ============================================================================

class Table {
public:
    Table(std::string name, Schema schema)
        : name_(std::move(name)), schema_(std::move(schema)) {
        columns_.reserve(schema_.size());
        for (size_t i = 0; i < schema_.size(); ++i)
            columns_.emplace_back(schema_[i].type);
    }

    const std::string& name()   const noexcept { return name_; }
    const Schema&      schema() const noexcept { return schema_; }
    size_t             rowCount() const noexcept {
        return columns_.empty() ? 0 : columns_[0].size();
    }
    size_t columnCount() const noexcept { return columns_.size(); }

    const Column& columnAt(size_t i) const { return columns_[i]; }
    Column&       columnAt(size_t i)       { return columns_[i]; }

    // Column index by (case-insensitive) name, or -1.
    int columnIndex(std::string_view name) const noexcept {
        return schema_.indexOf(name);
    }

    // Append one row. `row.size()` must equal the column count.
    void insertRow(Row row) {
        assert(row.size() == columns_.size());
        for (size_t c = 0; c < columns_.size(); ++c)
            columns_[c].append(row[c]);
    }

    // Bulk load: append every row of a column-major buffer. Each inner vector
    // is one full column and all must have identical length.
    void bulkInsertColumns(std::vector<std::vector<Value>> cols) {
        assert(cols.size() == columns_.size());
        for (size_t c = 0; c < columns_.size(); ++c) {
            columns_[c].reserve(columns_[c].size() + cols[c].size());
            for (const auto& v : cols[c])
                columns_[c].append(v);
        }
    }

    // Materialize row `r` as a vector of Values (used by row-oriented operators
    // such as joins and sorts; column scans avoid this).
    Row getRow(size_t r) const {
        Row out;
        out.reserve(columns_.size());
        for (const auto& col : columns_)
            out.push_back(col[r]);
        return out;
    }

private:
    std::string         name_;
    Schema              schema_;
    std::vector<Column> columns_;
};

using TablePtr = std::shared_ptr<Table>;

}  // namespace lq
