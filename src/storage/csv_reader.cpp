// ============================================================================
// LiteQuery — csv_reader.cpp
// ============================================================================

#include "csv_reader.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace lq {

namespace {

// ---- Field-level parsing ---------------------------------------------------

// Parse the whole text into a grid of string fields. Handles quoting,
// embedded delimiters/newlines inside quotes, "" escapes, and CRLF.
std::vector<std::vector<std::string>> parseGrid(const std::string& text,
                                                const CsvOptions& opts) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    bool fieldStarted = false;   // distinguishes an empty line from a 1-empty-field row

    size_t i = 0;
    const size_t n = text.size();

    // Skip a leading UTF-8 BOM.
    if (n >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        i = 3;
    }

    auto endField = [&]() { row.push_back(std::move(field)); field.clear(); fieldStarted = false; };
    auto endRow   = [&]() {
        endField();
        rows.push_back(std::move(row));
        row.clear();
    };

    for (; i < n; ++i) {
        char c = text[i];

        if (inQuotes) {
            if (c == opts.quote) {
                if (i + 1 < n && text[i + 1] == opts.quote) { field += opts.quote; ++i; }
                else inQuotes = false;
            } else {
                field += c;
            }
            continue;
        }

        if (c == opts.quote) { inQuotes = true; fieldStarted = true; continue; }
        if (c == opts.delimiter) { endField(); fieldStarted = true; continue; }
        if (c == '\r') { continue; }            // swallow CR; LF ends the row
        if (c == '\n') {
            // Every LF terminates a record. A blank line therefore becomes a
            // one-field record with an empty (NULL) value — matching CSV rules.
            // Trailing blank lines are trimmed after parsing (below).
            endRow();
            continue;
        }
        field += c;
        fieldStarted = true;
    }
    // Flush a trailing field/row only if the last line had content (no dangling
    // empty record from a final newline).
    if (fieldStarted || !field.empty() || !row.empty())
        endRow();

    // Drop trailing all-empty records (e.g. a file ending in "\n\n").
    while (!rows.empty()) {
        const auto& last = rows.back();
        bool allEmpty = true;
        for (const auto& f : last) if (!f.empty()) { allEmpty = false; break; }
        if (allEmpty && last.size() <= 1) rows.pop_back();
        else break;
    }

    return rows;
}

// ---- Type inference --------------------------------------------------------

bool isBlank(const std::string& s) {
    for (char c : s) if (!std::isspace((unsigned char)c)) return false;
    return true;
}

bool parsesAsInt(const std::string& s, int64_t& out) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    if (a == std::string::npos) return false;
    std::string t = s.substr(a, b - a + 1);
    const char* begin = t.data();
    const char* end   = t.data() + t.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool parsesAsDouble(const std::string& s, double& out) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    if (a == std::string::npos) return false;
    std::string t = s.substr(a, b - a + 1);
    try {
        size_t consumed = 0;
        out = std::stod(t, &consumed);
        return consumed == t.size();
    } catch (...) { return false; }
}

enum class Inferred { Int, Double, Text };

// Widening lattice: Int ⊂ Double ⊂ Text. Blank cells don't widen.
Inferred inferColumn(const std::vector<std::vector<std::string>>& grid,
                     size_t colIdx, size_t startRow, size_t inferRows) {
    Inferred t = Inferred::Int;
    bool sawAny = false;
    size_t limit = (inferRows == 0) ? grid.size()
                                    : std::min(grid.size(), startRow + inferRows);
    for (size_t r = startRow; r < limit; ++r) {
        if (colIdx >= grid[r].size()) continue;
        const std::string& cell = grid[r][colIdx];
        if (isBlank(cell)) continue;         // NULL — does not affect the type
        sawAny = true;
        int64_t iv; double dv;
        if (t == Inferred::Int) {
            if (parsesAsInt(cell, iv))       continue;
            if (parsesAsDouble(cell, dv))    { t = Inferred::Double; continue; }
            t = Inferred::Text; break;
        }
        if (t == Inferred::Double) {
            if (parsesAsDouble(cell, dv))    continue;
            t = Inferred::Text; break;
        }
    }
    if (!sawAny) return Inferred::Text;      // all-empty column → VARCHAR
    return t;
}

Value cellToValue(const std::string& cell, Inferred t) {
    if (isBlank(cell)) return Value::null();
    if (t == Inferred::Int)    { int64_t v; parsesAsInt(cell, v);    return Value(v); }
    if (t == Inferred::Double) { double  v; parsesAsDouble(cell, v); return Value(v); }
    return Value(cell);
}

}  // namespace

// ============================================================================
// Public entry points
// ============================================================================

TablePtr readCsvString(const std::string& text,
                       const std::string& tableName,
                       const CsvOptions& opts) {
    auto grid = parseGrid(text, opts);
    if (grid.empty())
        throw CsvError("CSV is empty: " + tableName);

    // Determine column names and the first data row.
    std::vector<std::string> names;
    size_t dataStart = 0;
    if (opts.hasHeader) {
        names = grid[0];
        dataStart = 1;
    } else {
        for (size_t c = 0; c < grid[0].size(); ++c)
            names.push_back("col" + std::to_string(c + 1));
    }
    const size_t ncol = names.size();
    if (ncol == 0) throw CsvError("CSV has no columns: " + tableName);

    // Infer each column's type.
    std::vector<Inferred> types(ncol);
    for (size_t c = 0; c < ncol; ++c)
        types[c] = inferColumn(grid, c, dataStart, opts.inferRows);

    // Build the schema.
    Schema schema;
    for (size_t c = 0; c < ncol; ++c) {
        DataType dt = types[c] == Inferred::Int    ? DataType::int64()
                    : types[c] == Inferred::Double ? DataType::float64()
                                                   : DataType::varchar();
        // CSV columns are always nullable (blank cells → NULL).
        schema.addColumn(names[c].empty() ? ("col" + std::to_string(c + 1)) : names[c], dt);
    }

    // Materialize the data column-major for a bulk load.
    std::vector<std::vector<Value>> columns(ncol);
    for (auto& col : columns) col.reserve(grid.size() - dataStart);

    for (size_t r = dataStart; r < grid.size(); ++r) {
        const auto& record = grid[r];
        if (record.size() != ncol) {
            // Be forgiving: pad short rows with NULLs, ignore extra trailing cells.
            for (size_t c = 0; c < ncol; ++c) {
                if (c < record.size()) columns[c].push_back(cellToValue(record[c], types[c]));
                else                   columns[c].push_back(Value::null());
            }
            continue;
        }
        for (size_t c = 0; c < ncol; ++c)
            columns[c].push_back(cellToValue(record[c], types[c]));
    }

    auto table = std::make_shared<Table>(tableName, std::move(schema));
    table->bulkInsertColumns(std::move(columns));
    return table;
}

TablePtr readCsv(const std::string& path,
                 const std::string& tableName,
                 const CsvOptions& opts) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw CsvError("cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return readCsvString(ss.str(), tableName, opts);
}

}  // namespace lq
