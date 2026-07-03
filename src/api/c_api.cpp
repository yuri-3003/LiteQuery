// ============================================================================
// LiteQuery — c_api.cpp
// extern "C" wrapper over the C++ Connection. No exception ever crosses the
// boundary: every entry point is wrapped so a thrown std::exception becomes an
// error stored on the handle (or a status code), never propagation into C.
// ============================================================================

#include "litequery/litequery.h"

#include "connection.h"

#include <exception>
#include <new>
#include <string>

// ============================================================================
// Handle definitions (opaque to C)
// ============================================================================

struct lq_db {
    lq::Connection conn;
};

struct lq_result {
    lq::QueryResult result;
    // Iteration cursor: -1 = before first row (matches lq_result_next contract).
    long long cursor = -1;
    // Scratch buffer so lq_result_get_text returns a stable const char*.
    std::string textScratch;
    // Storage for the error string returned via lq_exec's out_error.
    std::string errorScratch;
};

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Guard a block that returns lq_status, converting any exception to LQ_ERROR.
#define LQ_GUARD_STATUS(body)                     \
    try { body }                                  \
    catch (const std::exception&) { return LQ_ERROR; } \
    catch (...) { return LQ_ERROR; }

// True if `res` has a valid current row.
bool hasCurrentRow(const lq_result* res) {
    return res && res->cursor >= 0 &&
           static_cast<size_t>(res->cursor) < res->result.rows.size();
}

const lq::Value* currentValue(const lq_result* res, size_t col) {
    if (!hasCurrentRow(res)) return nullptr;
    const lq::Row& row = res->result.rows[static_cast<size_t>(res->cursor)];
    if (col >= row.size()) return nullptr;
    return &row[col];
}

}  // namespace

// ============================================================================
// Version
// ============================================================================

extern "C" const char* lq_version(void) {
    return LITEQUERY_VERSION_STRING;
}

// ============================================================================
// Database lifecycle
// ============================================================================

extern "C" lq_db* lq_open(void) {
    try {
        return new lq_db();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void lq_close(lq_db* db) {
    delete db;   // safe on nullptr
}

// ============================================================================
// Query execution
// ============================================================================

extern "C" lq_result* lq_query(lq_db* db, const char* sql) {
    lq_result* res = nullptr;
    try {
        res = new lq_result();
        if (!db || !sql) {
            res->result = lq::QueryResult::makeError("null db or sql");
            return res;
        }
        res->result = db->conn.query(sql);
    } catch (const std::exception& e) {
        if (res) res->result = lq::QueryResult::makeError(e.what());
    } catch (...) {
        if (res) res->result = lq::QueryResult::makeError("unknown error");
    }
    return res;
}

extern "C" lq_status lq_exec(lq_db* db, const char* sql,
                             int64_t* affected, const char** out_error) {
    if (!db || !sql) return LQ_MISUSE;
    LQ_GUARD_STATUS({
        lq::QueryResult r = db->conn.query(sql);
        if (!r.ok()) {
            if (out_error) {
                // Leak-free static-thread-local storage for the message.
                thread_local std::string err;
                err = r.errorMessage;
                *out_error = err.c_str();
            }
            return LQ_ERROR;
        }
        if (affected) *affected = r.rowsAffected;
        return LQ_OK;
    });
}

extern "C" lq_status lq_import_csv(lq_db* db, const char* path, const char* table_name,
                                   char delimiter, int has_header,
                                   int64_t* rows, const char** out_error) {
    if (!db || !path || !table_name) return LQ_MISUSE;
    LQ_GUARD_STATUS({
        lq::QueryResult r = db->conn.importCsv(path, table_name,
                                               delimiter ? delimiter : ',',
                                               has_header != 0);
        if (!r.ok()) {
            if (out_error) {
                thread_local std::string err;
                err = r.errorMessage;
                *out_error = err.c_str();
            }
            return LQ_ERROR;
        }
        if (rows) *rows = r.rowsAffected;
        return LQ_OK;
    });
}

// ============================================================================
// Result inspection
// ============================================================================

extern "C" int lq_result_ok(const lq_result* res) {
    return (res && res->result.ok()) ? 1 : 0;
}

extern "C" const char* lq_result_error(const lq_result* res) {
    if (!res) return "null result";
    return res->result.errorMessage.c_str();
}

extern "C" size_t lq_result_column_count(const lq_result* res) {
    return res ? res->result.schema.size() : 0;
}

extern "C" size_t lq_result_row_count(const lq_result* res) {
    return res ? res->result.rows.size() : 0;
}

extern "C" int64_t lq_result_rows_affected(const lq_result* res) {
    return res ? res->result.rowsAffected : 0;
}

extern "C" int64_t lq_result_elapsed_micros(const lq_result* res) {
    return res ? res->result.elapsedMicros : 0;
}

extern "C" const char* lq_result_column_name(const lq_result* res, size_t col) {
    if (!res || col >= res->result.schema.size()) return "";
    return res->result.schema[col].name.c_str();
}

// ============================================================================
// Row iteration & value access
// ============================================================================

extern "C" int lq_result_next(lq_result* res) {
    if (!res || !res->result.ok()) return 0;
    if (res->cursor + 1 >= static_cast<long long>(res->result.rows.size()))
        { res->cursor = static_cast<long long>(res->result.rows.size()); return 0; }
    ++res->cursor;
    return 1;
}

extern "C" void lq_result_reset(lq_result* res) {
    if (res) res->cursor = -1;
}

extern "C" lq_type lq_result_column_type(const lq_result* res, size_t col) {
    const lq::Value* v = currentValue(res, col);
    if (!v || v->isNull()) return LQ_TYPE_NULL;
    switch (v->typeId()) {
        case lq::TypeId::BOOLEAN: return LQ_TYPE_BOOL;
        case lq::TypeId::INT8: case lq::TypeId::INT16:
        case lq::TypeId::INT32: case lq::TypeId::INT64: return LQ_TYPE_INT;
        case lq::TypeId::FLOAT32: case lq::TypeId::FLOAT64: return LQ_TYPE_DOUBLE;
        case lq::TypeId::VARCHAR: return LQ_TYPE_TEXT;
        default: return LQ_TYPE_TEXT;
    }
}

extern "C" int lq_result_is_null(const lq_result* res, size_t col) {
    const lq::Value* v = currentValue(res, col);
    return (!v || v->isNull()) ? 1 : 0;
}

extern "C" lq_status lq_result_get_int(const lq_result* res, size_t col, int64_t* out) {
    if (!out) return LQ_MISUSE;
    if (!hasCurrentRow(res)) return LQ_ERROR;
    const lq::Value* v = currentValue(res, col);
    if (!v) return LQ_RANGE;
    LQ_GUARD_STATUS({
        *out = v->isNull() ? 0 : (lq::typeIsInteger(v->typeId())
                                    ? v->toInt64()
                                    : static_cast<int64_t>(v->toDouble()));
        return LQ_OK;
    });
}

extern "C" lq_status lq_result_get_double(const lq_result* res, size_t col, double* out) {
    if (!out) return LQ_MISUSE;
    if (!hasCurrentRow(res)) return LQ_ERROR;
    const lq::Value* v = currentValue(res, col);
    if (!v) return LQ_RANGE;
    LQ_GUARD_STATUS({
        *out = v->isNull() ? 0.0 : v->toDouble();
        return LQ_OK;
    });
}

extern "C" lq_status lq_result_get_bool(const lq_result* res, size_t col, int* out) {
    if (!out) return LQ_MISUSE;
    if (!hasCurrentRow(res)) return LQ_ERROR;
    const lq::Value* v = currentValue(res, col);
    if (!v) return LQ_RANGE;
    LQ_GUARD_STATUS({
        if (v->isNull()) { *out = 0; return LQ_OK; }
        if (v->typeId() == lq::TypeId::BOOLEAN) *out = v->getBool() ? 1 : 0;
        else *out = (v->toDouble() != 0.0) ? 1 : 0;
        return LQ_OK;
    });
}

extern "C" const char* lq_result_get_text(const lq_result* res, size_t col) {
    const lq::Value* v = currentValue(res, col);
    if (!v || v->isNull()) return "";
    // Mutate the scratch buffer on the (non-const) handle. The public contract
    // documents that the returned pointer is valid until the next call.
    lq_result* mres = const_cast<lq_result*>(res);
    try {
        mres->textScratch = (v->typeId() == lq::TypeId::VARCHAR)
                                ? v->getString()
                                : v->toString();
    } catch (...) {
        mres->textScratch.clear();
    }
    return mres->textScratch.c_str();
}

extern "C" void lq_result_free(lq_result* res) {
    delete res;   // safe on nullptr
}
