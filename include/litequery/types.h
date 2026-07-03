#pragma once

// ============================================================================
// LiteQuery — types.h
// Core type system: TypeId, DataType, Value, Batch
//
// This header is internal — not part of the public C API (litequery.h).
// All query engine layers (parser, planner, executor, storage) depend on this.
// Keep it free of heavy includes; everything here must compile fast.
// ============================================================================

#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lq {

// ============================================================================
// TypeId — discriminator for every type the engine understands
// ============================================================================

enum class TypeId : uint8_t {
    INVALID  = 0,  // Unresolved / sentinel — must stay zero
    BOOLEAN,       // 1-bit logical value stored as uint8_t
    INT8,          // Signed 8-bit integer
    INT16,         // Signed 16-bit integer
    INT32,         // Signed 32-bit integer  (default integer literal type)
    INT64,         // Signed 64-bit integer
    FLOAT32,       // IEEE-754 single precision
    FLOAT64,       // IEEE-754 double precision (default float literal type)
    VARCHAR,       // Variable-length UTF-8 string (max 1 GB)
    BLOB,          // Raw bytes
    DATE,          // Days since Unix epoch (stored as int32_t)
    TIMESTAMP,     // Microseconds since Unix epoch (stored as int64_t)
    INTERVAL,      // Duration in microseconds (stored as int64_t)
    DECIMAL,       // Fixed-point: int64_t mantissa + uint8_t scale
};

// Human-readable name for error messages and SHOW COLUMNS output.
inline constexpr std::string_view typeIdName(TypeId id) noexcept {
    switch (id) {
        case TypeId::INVALID:   return "INVALID";
        case TypeId::BOOLEAN:   return "BOOLEAN";
        case TypeId::INT8:      return "INT8";
        case TypeId::INT16:     return "INT16";
        case TypeId::INT32:     return "INT32";
        case TypeId::INT64:     return "INT64";
        case TypeId::FLOAT32:   return "FLOAT32";
        case TypeId::FLOAT64:   return "FLOAT64";
        case TypeId::VARCHAR:   return "VARCHAR";
        case TypeId::BLOB:      return "BLOB";
        case TypeId::DATE:      return "DATE";
        case TypeId::TIMESTAMP: return "TIMESTAMP";
        case TypeId::INTERVAL:  return "INTERVAL";
        case TypeId::DECIMAL:   return "DECIMAL";
    }
    return "UNKNOWN";
}

// Physical byte width for fixed-width types; 0 for variable-width types.
inline constexpr size_t typeIdWidth(TypeId id) noexcept {
    switch (id) {
        case TypeId::BOOLEAN:   return 1;
        case TypeId::INT8:      return 1;
        case TypeId::INT16:     return 2;
        case TypeId::INT32:     return 4;
        case TypeId::INT64:     return 8;
        case TypeId::FLOAT32:   return 4;
        case TypeId::FLOAT64:   return 8;
        case TypeId::DATE:      return 4;
        case TypeId::TIMESTAMP: return 8;
        case TypeId::INTERVAL:  return 8;
        case TypeId::DECIMAL:   return 8;  // mantissa only; scale lives in DataType
        default:                return 0;  // VARCHAR, BLOB — variable width
    }
}

inline constexpr bool typeIsInteger(TypeId id) noexcept {
    return id >= TypeId::INT8 && id <= TypeId::INT64;
}

inline constexpr bool typeIsFloat(TypeId id) noexcept {
    return id == TypeId::FLOAT32 || id == TypeId::FLOAT64;
}

inline constexpr bool typeIsNumeric(TypeId id) noexcept {
    return typeIsInteger(id) || typeIsFloat(id) || id == TypeId::DECIMAL;
}

inline constexpr bool typeIsFixedWidth(TypeId id) noexcept {
    return typeIdWidth(id) > 0;
}

// ============================================================================
// DataType — TypeId + optional modifiers (precision, scale, max-length, nullability)
// ============================================================================

struct DataType {
    TypeId  id       = TypeId::INVALID;
    bool    nullable = true;   // True unless explicitly NOT NULL
    uint8_t scale    = 0;      // DECIMAL: digits right of decimal point
    uint8_t precision = 0;     // DECIMAL: total significant digits
    int32_t maxLength = -1;    // VARCHAR(N): max bytes; -1 = unbounded

    // Convenience constructors for common types
    static DataType boolean(bool nullable = true) {
        return {TypeId::BOOLEAN, nullable};
    }
    static DataType int32(bool nullable = true) {
        return {TypeId::INT32, nullable};
    }
    static DataType int64(bool nullable = true) {
        return {TypeId::INT64, nullable};
    }
    static DataType float64(bool nullable = true) {
        return {TypeId::FLOAT64, nullable};
    }
    static DataType varchar(int32_t maxLen = -1, bool nullable = true) {
        DataType dt{TypeId::VARCHAR, nullable};
        dt.maxLength = maxLen;
        return dt;
    }
    static DataType decimal(uint8_t precision, uint8_t scale, bool nullable = true) {
        DataType dt{TypeId::DECIMAL, nullable};
        dt.precision = precision;
        dt.scale     = scale;
        return dt;
    }
    static DataType date(bool nullable = true) {
        return {TypeId::DATE, nullable};
    }
    static DataType timestamp(bool nullable = true) {
        return {TypeId::TIMESTAMP, nullable};
    }

    bool operator==(const DataType& o) const noexcept {
        return id == o.id && nullable == o.nullable &&
               scale == o.scale && precision == o.precision &&
               maxLength == o.maxLength;
    }
    bool operator!=(const DataType& o) const noexcept { return !(*this == o); }

    // Returns the same type but nullable — used during type resolution for
    // expressions that can introduce NULLs (e.g. outer join, CASE).
    DataType asNullable() const noexcept {
        DataType copy = *this;
        copy.nullable = true;
        return copy;
    }

    std::string toString() const {
        std::string s(typeIdName(id));
        if (id == TypeId::VARCHAR && maxLength >= 0)
            s += "(" + std::to_string(maxLength) + ")";
        if (id == TypeId::DECIMAL)
            s += "(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
        if (!nullable) s += " NOT NULL";
        return s;
    }
};

// ============================================================================
// NullValue — a typed sentinel that fits inside Value below
// ============================================================================

struct NullValue {
    bool operator==(const NullValue&) const noexcept { return true; }
    bool operator!=(const NullValue&) const noexcept { return false; }
};

// ============================================================================
// Value — a single scalar that can hold any supported type or NULL
//
// Design notes:
//   - std::variant chosen over a tagged union for type safety and because
//     Value appears in optimizer / planner layers where the overhead is fine.
//   - Hot executor paths work directly on typed Column<T> arrays, not Values.
//   - std::string is used for VARCHAR/BLOB. For large BLOBs in production
//     you would replace this with a string_view into a separately managed
//     arena — acceptable future optimisation.
// ============================================================================

using ValueVariant = std::variant<
    NullValue,
    bool,
    int8_t,
    int16_t,
    int32_t,
    int64_t,
    float,
    double,
    std::string   // VARCHAR and BLOB both land here
>;

class Value {
public:
    // ---- Construction -------------------------------------------------------

    Value() noexcept : data_(NullValue{}) {}  // Default: NULL

    explicit Value(bool v)     noexcept : data_(v) {}
    explicit Value(int8_t v)   noexcept : data_(v) {}
    explicit Value(int16_t v)  noexcept : data_(v) {}
    explicit Value(int32_t v)  noexcept : data_(v) {}
    explicit Value(int64_t v)  noexcept : data_(v) {}
    explicit Value(float v)    noexcept : data_(v) {}
    explicit Value(double v)   noexcept : data_(v) {}
    explicit Value(std::string v)      : data_(std::move(v)) {}
    explicit Value(std::string_view v) : data_(std::string(v)) {}
    explicit Value(const char* v)      : data_(std::string(v)) {}

    static Value null() noexcept { return Value{}; }

    // ---- Null checks --------------------------------------------------------

    bool isNull() const noexcept {
        return std::holds_alternative<NullValue>(data_);
    }

    // ---- Type query ---------------------------------------------------------

    TypeId typeId() const noexcept {
        return std::visit([](const auto& v) -> TypeId {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, NullValue>)   return TypeId::INVALID;
            if constexpr (std::is_same_v<T, bool>)        return TypeId::BOOLEAN;
            if constexpr (std::is_same_v<T, int8_t>)      return TypeId::INT8;
            if constexpr (std::is_same_v<T, int16_t>)     return TypeId::INT16;
            if constexpr (std::is_same_v<T, int32_t>)     return TypeId::INT32;
            if constexpr (std::is_same_v<T, int64_t>)     return TypeId::INT64;
            if constexpr (std::is_same_v<T, float>)       return TypeId::FLOAT32;
            if constexpr (std::is_same_v<T, double>)      return TypeId::FLOAT64;
            if constexpr (std::is_same_v<T, std::string>) return TypeId::VARCHAR;
            return TypeId::INVALID;
        }, data_);
    }

    // ---- Typed accessors (throw on type mismatch) ---------------------------

    template <typename T>
    const T& get() const {
        if (!std::holds_alternative<T>(data_)) {
            throw std::bad_variant_access{};
        }
        return std::get<T>(data_);
    }

    template <typename T>
    T& get() {
        if (!std::holds_alternative<T>(data_)) {
            throw std::bad_variant_access{};
        }
        return std::get<T>(data_);
    }

    // Convenient typed helpers
    bool        getBool()   const { return get<bool>(); }
    int8_t      getInt8()   const { return get<int8_t>(); }
    int16_t     getInt16()  const { return get<int16_t>(); }
    int32_t     getInt32()  const { return get<int32_t>(); }
    int64_t     getInt64()  const { return get<int64_t>(); }
    float       getFloat()  const { return get<float>(); }
    double      getDouble() const { return get<double>(); }
    const std::string& getString() const { return get<std::string>(); }

    // Cast to int64/double for arithmetic without knowing exact subtype.
    // Throws if the value is NULL or a non-numeric type.
    int64_t toInt64() const {
        return std::visit([](const auto& v) -> int64_t {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_integral_v<T> && !std::is_same_v<T, NullValue>)
                return static_cast<int64_t>(v);
            throw std::runtime_error("Value::toInt64: not an integer type");
        }, data_);
    }

    double toDouble() const {
        return std::visit([](const auto& v) -> double {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, NullValue>)
                return static_cast<double>(v);
            throw std::runtime_error("Value::toDouble: not a numeric type");
        }, data_);
    }

    // ---- Comparison ---------------------------------------------------------
    // NULL propagation: any comparison involving NULL returns false (SQL rules).

    bool operator==(const Value& o) const noexcept {
        if (isNull() || o.isNull()) return false;
        return data_ == o.data_;
    }
    bool operator!=(const Value& o) const noexcept {
        if (isNull() || o.isNull()) return false;
        return data_ != o.data_;
    }
    bool operator<(const Value& o) const {
        if (isNull() || o.isNull()) return false;
        return std::visit([&]<typename L>(const L& lhs) -> bool {
            return std::visit([&]<typename R>(const R& rhs) -> bool {
                if constexpr (std::is_same_v<L, R> && !std::is_same_v<L, NullValue>)
                    return lhs < rhs;
                throw std::runtime_error("Value: incompatible types in comparison");
            }, o.data_);
        }, data_);
    }
    bool operator<=(const Value& o) const { return !(o < *this); }
    bool operator> (const Value& o) const { return o < *this; }
    bool operator>=(const Value& o) const { return !(*this < o); }

    // NULL-aware equality: NULL IS NULL → true (for GROUP BY / DISTINCT keys)
    bool isIdenticalTo(const Value& o) const noexcept {
        if (isNull() && o.isNull()) return true;
        if (isNull() || o.isNull()) return false;
        return data_ == o.data_;
    }

    // ---- Display ------------------------------------------------------------

    std::string toString() const {
        return std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, NullValue>)   return "NULL";
            if constexpr (std::is_same_v<T, bool>)        return v ? "true" : "false";
            if constexpr (std::is_same_v<T, std::string>) return v;
            if constexpr (std::is_arithmetic_v<T>)        return std::to_string(v);
            return "";
        }, data_);
    }

private:
    ValueVariant data_;
};

// ============================================================================
// ColumnDef — one column in a schema (name + type)
// ============================================================================

struct ColumnDef {
    std::string name;
    DataType    type;

    ColumnDef() = default;
    ColumnDef(std::string name, DataType type)
        : name(std::move(name)), type(std::move(type)) {}

    bool operator==(const ColumnDef& o) const noexcept {
        return name == o.name && type == o.type;
    }
};

// ============================================================================
// Schema — ordered list of ColumnDefs describing a table or result set
// ============================================================================

class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<ColumnDef> cols) : cols_(std::move(cols)) {}

    // Build incrementally
    Schema& addColumn(std::string name, DataType type) {
        cols_.push_back({std::move(name), std::move(type)});
        return *this;
    }

    size_t size() const noexcept { return cols_.size(); }
    bool   empty() const noexcept { return cols_.empty(); }

    const ColumnDef& operator[](size_t i) const {
        assert(i < cols_.size());
        return cols_[i];
    }

    const std::vector<ColumnDef>& columns() const noexcept { return cols_; }

    // Returns column index or -1 if not found. Case-insensitive.
    int indexOf(std::string_view name) const noexcept {
        for (int i = 0; i < static_cast<int>(cols_.size()); ++i) {
            if (cols_[i].name.size() == name.size() &&
                strncasecmp(cols_[i].name.c_str(), name.data(), name.size()) == 0)
                return i;
        }
        return -1;
    }

    // Return a new schema keeping only the columns at the given indices.
    Schema project(const std::vector<size_t>& indices) const {
        Schema out;
        for (size_t idx : indices) {
            assert(idx < cols_.size());
            out.cols_.push_back(cols_[idx]);
        }
        return out;
    }

    // Concatenate two schemas (used after a join to form the output schema).
    static Schema merge(const Schema& left, const Schema& right) {
        Schema out = left;
        out.cols_.insert(out.cols_.end(),
                         right.cols_.begin(), right.cols_.end());
        return out;
    }

    bool operator==(const Schema& o) const noexcept { return cols_ == o.cols_; }

private:
    std::vector<ColumnDef> cols_;
};

// ============================================================================
// Row — a vector of Values; used in result sets and hash table payloads
// ============================================================================

using Row = std::vector<Value>;

// ============================================================================
// Batch — columnar slice passed between executor operators
//
// Each Batch holds a fixed number of rows stored column-by-column.
// Operators call next() to pull one Batch at a time from their child.
// Batch size defaults to 1024 rows — large enough to amortise call overhead,
// small enough to fit L1/L2 cache for a few integer columns.
//
// Note: this is a simple Value-based Batch used in the first milestone.
// The vectorized executor milestone replaces the inner vectors with typed
// Arrow-style buffers (uint8_t*, int32_t*, int64_t*) and a validity bitmap.
// ============================================================================

static constexpr size_t kDefaultBatchSize = 1024;

struct Batch {
    Schema                        schema;
    std::vector<std::vector<Value>> columns;  // columns[col_idx][row_idx]
    size_t                        numRows = 0;

    Batch() = default;
    explicit Batch(Schema s) : schema(std::move(s)) {
        columns.resize(schema.size());
    }

    bool empty() const noexcept { return numRows == 0; }

    // Append a single row expressed as a vector of Values.
    void appendRow(Row row) {
        assert(row.size() == schema.size());
        for (size_t c = 0; c < columns.size(); ++c)
            columns[c].push_back(std::move(row[c]));
        ++numRows;
    }

    // Get a single value by (row, column) coordinates.
    const Value& at(size_t row, size_t col) const {
        assert(col < columns.size() && row < numRows);
        return columns[col][row];
    }

    // Extract a row as a vector of Values (useful for hash table keys).
    Row getRow(size_t row) const {
        Row r;
        r.reserve(columns.size());
        for (const auto& col : columns)
            r.push_back(col[row]);
        return r;
    }
};

// ============================================================================
// Type promotion rules for binary arithmetic / comparison expressions
//
// Returns the type that both operands should be cast to before the operation.
// Follows standard SQL implicit conversion rules:
//   integer < float < double
//   narrower integer < wider integer
// Returns TypeId::INVALID if the combination is unsupported.
// ============================================================================

inline TypeId promoteTypes(TypeId a, TypeId b) noexcept {
    if (a == b) return a;

    // NULL propagation: if either side is INVALID (NULL-typed), keep the other
    if (a == TypeId::INVALID) return b;
    if (b == TypeId::INVALID) return a;

    // Float wins over any integer
    if (a == TypeId::FLOAT64 || b == TypeId::FLOAT64) return TypeId::FLOAT64;
    if (a == TypeId::FLOAT32 || b == TypeId::FLOAT32) return TypeId::FLOAT64; // widen
    if (a == TypeId::DECIMAL || b == TypeId::DECIMAL)  return TypeId::DECIMAL;

    // Both integer: pick the wider one
    if (typeIsInteger(a) && typeIsInteger(b))
        return (typeIdWidth(a) >= typeIdWidth(b)) ? a : b;

    return TypeId::INVALID;  // Incompatible — caller must raise a type error
}

// ============================================================================
// Common exception types
// ============================================================================

struct TypeError : std::runtime_error {
    explicit TypeError(const std::string& msg) : std::runtime_error(msg) {}
};

struct CastError : std::runtime_error {
    explicit CastError(const std::string& msg) : std::runtime_error(msg) {}
};

}  // namespace lq