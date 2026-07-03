#pragma once

// ============================================================================
// LiteQuery — table.h
// Column-oriented in-memory table storage.
//
// A Table owns one Column per schema field. Each Column stores its values
// contiguously (std::vector<Value>) so scans over a single column are cache-
// friendly and vectorizable — the essence of the "columnar" execution model.
//
// This is the MVP storage form: values are boxed in lq::Value. The performance
// milestone described in the design docs replaces each Column with a typed,
// page-based buffer + Arrow-style validity bitmap; the Table/TableScan API
// below is deliberately shaped so that swap can happen without touching the
// executor (operators only ever call scan()/columnAt()/rowCount()).
//
// Concurrency: a Table is not internally synchronized. The Catalog guards
// table lookup; callers serialize writes to a given table.
// ============================================================================

#include "types.h"

#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lq {

// ============================================================================
// Column — a single typed column of values
// ============================================================================

class Column {
public:
    explicit Column(DataType type) : type_(std::move(type)) {}

    const DataType& type() const noexcept { return type_; }
    size_t          size() const noexcept { return data_.size(); }

    void append(Value v) { data_.push_back(std::move(v)); }
    void reserve(size_t n) { data_.reserve(n); }

    const Value& operator[](size_t i) const { return data_[i]; }
    Value&       operator[](size_t i)       { return data_[i]; }

    const std::vector<Value>& values() const noexcept { return data_; }
    std::vector<Value>&       values()       noexcept { return data_; }

private:
    DataType           type_;
    std::vector<Value> data_;
};

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
            columns_[c].append(std::move(row[c]));
    }

    // Bulk load: append every row of a column-major buffer. Each inner vector
    // is one full column and all must have identical length.
    void bulkInsertColumns(std::vector<std::vector<Value>> cols) {
        assert(cols.size() == columns_.size());
        for (size_t c = 0; c < columns_.size(); ++c) {
            auto& dst = columns_[c].values();
            dst.insert(dst.end(),
                       std::make_move_iterator(cols[c].begin()),
                       std::make_move_iterator(cols[c].end()));
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
