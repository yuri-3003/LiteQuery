#pragma once

// ============================================================================
// LiteQuery — csv_reader.h
// Load a delimited text file (CSV/TSV) into a columnar Table.
//
// Pipeline:
//   1. Split the file into records, honoring RFC-4180 quoting ("a,b", ""
//      escapes), embedded newlines inside quotes, CRLF line endings, and a
//      leading UTF-8 BOM.
//   2. Take the header row as column names (or synthesize col1, col2, … when
//      the file has no header).
//   3. Infer each column's type by scanning its values: a column is INT64 if
//      every non-empty value parses as an integer, else FLOAT64 if every
//      non-empty value parses as a double, else VARCHAR. Empty fields become
//      NULL and never force a widening.
//   4. Materialize a Table, column by column (bulk load, not row-by-row).
//
// This is intentionally dependency-free and streaming-friendly for the common
// case; very large files still fit the same API (the whole file is read into
// memory in this MVP — chunked ingestion is a later milestone).
// ============================================================================

#include "table.h"
#include "types.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace lq {

struct CsvError : std::runtime_error {
    explicit CsvError(const std::string& msg) : std::runtime_error(msg) {}
};

// Options controlling how a file is parsed.
struct CsvOptions {
    char delimiter   = ',';    // ',' for CSV, '\t' for TSV
    bool hasHeader   = true;   // first record names the columns
    char quote       = '"';
    // Maximum rows scanned for type inference (0 = all rows).
    size_t inferRows = 0;
};

// Parse the file at `path` and return a populated Table named `tableName`.
// Throws CsvError on I/O failure or a ragged file (row width != header width).
TablePtr readCsv(const std::string& path,
                 const std::string& tableName,
                 const CsvOptions& opts = {});

// Parse CSV text already in memory (used by tests and read_csv()).
TablePtr readCsvString(const std::string& text,
                       const std::string& tableName,
                       const CsvOptions& opts = {});

}  // namespace lq
