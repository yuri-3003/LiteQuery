#pragma once

// ============================================================================
// LiteQuery — column.h
// Typed, contiguous column storage with an Arrow-style validity bitmap.
//
// A Column stores its values in a *typed* buffer (e.g. std::vector<int64_t>)
// rather than boxed lq::Value objects, plus a 1-bit-per-row validity bitmap
// marking NULLs. This is the columnar performance foundation:
//
//   - a FLOAT64 column is 8 bytes/value + 1 bit, versus ~40 bytes boxed;
//   - scans and aggregations read a flat array, so they vectorize and stay in
//     cache instead of chasing std::variant discriminants per element.
//
// The public API is deliberately the same shape the boxed Column exposed
// (append(Value)/operator[]→Value/reserve/size) so existing callers — the
// executor's row-oriented operators, tests, and CSV bulk load — keep working
// unchanged. Hot paths additionally use the typed accessors (i64()/f64()/…)
// and validity() to avoid boxing entirely.
// ============================================================================

#include "types.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace lq {

// ============================================================================
// ValidityBitmap — 1 bit per row; 1 = valid (non-null), 0 = null.
// ============================================================================

class ValidityBitmap {
public:
    void reserve(size_t n) { bits_.reserve((n + 63) / 64); }

    size_t size() const noexcept { return count_; }

    void push(bool valid) {
        size_t word = count_ >> 6;
        if (word >= bits_.size()) bits_.push_back(0);
        if (valid) bits_[word] |= (uint64_t{1} << (count_ & 63));
        ++count_;
        if (!valid) anyNull_ = true;
    }

    bool isValid(size_t i) const noexcept {
        return (bits_[i >> 6] >> (i & 63)) & 1u;
    }

    // True if the column has at least one NULL (lets hot paths skip null checks).
    bool anyNull() const noexcept { return anyNull_; }

    void clear() noexcept { bits_.clear(); count_ = 0; anyNull_ = false; }

private:
    std::vector<uint64_t> bits_;
    size_t                count_   = 0;
    bool                  anyNull_ = false;
};

// ============================================================================
// Column — one typed column
//
// Storage is chosen by the column's TypeId:
//   BOOLEAN/INT8/16/32/64, DATE, TIMESTAMP  → int64_t buffer
//   FLOAT32/FLOAT64                          → double  buffer
//   VARCHAR/BLOB                             → std::string buffer
// (Narrow integer types are widened to int64 in-memory; the declared DataType
//  is preserved for schema/round-trip. This keeps the typed paths to two
//  numeric kinds, which is where the speed matters.)
// ============================================================================

class Column {
public:
    enum class Kind : uint8_t { Int64, Double, String };

    explicit Column(DataType type) : type_(std::move(type)), kind_(kindOf(type_.id)) {}

    const DataType& type() const noexcept { return type_; }
    Kind            kind() const noexcept { return kind_; }
    size_t          size() const noexcept { return validity_.size(); }

    void reserve(size_t n) {
        validity_.reserve(n);
        switch (kind_) {
            case Kind::Int64:  i64_.reserve(n);  break;
            case Kind::Double: f64_.reserve(n);  break;
            case Kind::String: str_.reserve(n);  break;
        }
    }

    // ---- Append (boxed) — the compatibility entry point --------------------
    void append(const Value& v) {
        if (v.isNull()) { pushNull(); return; }
        switch (kind_) {
            case Kind::Int64:
                i64_.push_back(typeIsFloat(v.typeId()) ? static_cast<int64_t>(v.toDouble())
                                                       : v.toInt64());
                break;
            case Kind::Double: f64_.push_back(v.toDouble()); break;
            case Kind::String: str_.push_back(v.typeId() == TypeId::VARCHAR
                                                 ? v.getString() : v.toString()); break;
        }
        validity_.push(true);
    }

    // ---- Typed append (hot path, no boxing) --------------------------------
    void appendInt64(int64_t v)             { i64_.push_back(v); validity_.push(true); }
    void appendDouble(double v)             { f64_.push_back(v); validity_.push(true); }
    void appendString(std::string v)        { str_.push_back(std::move(v)); validity_.push(true); }
    void pushNull() {
        switch (kind_) {
            case Kind::Int64:  i64_.push_back(0);        break;
            case Kind::Double: f64_.push_back(0.0);      break;
            case Kind::String: str_.emplace_back();      break;
        }
        validity_.push(false);
    }

    // ---- Boxed access (compat) --------------------------------------------
    Value operator[](size_t i) const { return valueAt(i); }

    Value valueAt(size_t i) const {
        if (!validity_.isValid(i)) return Value::null();
        switch (kind_) {
            case Kind::Int64:  return makeIntValue(i64_[i]);
            case Kind::Double: return Value(f64_[i]);
            case Kind::String: return Value(str_[i]);
        }
        return Value::null();
    }

    // ---- Typed raw access (hot path) --------------------------------------
    const std::vector<int64_t>&     i64() const noexcept { return i64_; }
    const std::vector<double>&      f64() const noexcept { return f64_; }
    const std::vector<std::string>& str() const noexcept { return str_; }
    const ValidityBitmap&           validity() const noexcept { return validity_; }
    bool isValid(size_t i) const noexcept { return validity_.isValid(i); }

    static Kind kindOf(TypeId id) {
        if (id == TypeId::FLOAT32 || id == TypeId::FLOAT64) return Kind::Double;
        if (id == TypeId::VARCHAR || id == TypeId::BLOB)    return Kind::String;
        return Kind::Int64;   // booleans, all ints, date, timestamp, interval, decimal mantissa
    }

private:
    // Reconstruct a Value of the column's declared integer subtype.
    Value makeIntValue(int64_t v) const {
        switch (type_.id) {
            case TypeId::BOOLEAN: return Value(static_cast<bool>(v));
            case TypeId::INT8:    return Value(static_cast<int8_t>(v));
            case TypeId::INT16:   return Value(static_cast<int16_t>(v));
            case TypeId::INT32:   return Value(static_cast<int32_t>(v));
            default:              return Value(static_cast<int64_t>(v));
        }
    }

    DataType                 type_;
    Kind                     kind_;
    std::vector<int64_t>     i64_;
    std::vector<double>      f64_;
    std::vector<std::string> str_;
    ValidityBitmap           validity_;
};

}  // namespace lq
