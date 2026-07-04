#pragma once

// ============================================================================
// LiteQuery — persistence.h
// Save/load a whole database (catalog of tables) to a single file.
//
// On-disk format (little-endian, all sizes are uint64):
//
//   "LQDB" magic (4 bytes)  |  format version (uint32)  |  table count (uint64)
//   for each table:
//     name  (len-prefixed UTF-8)
//     column count (uint64)
//     for each column:  name (len-prefixed) + DataType (typeid, nullable,
//                       scale, precision, maxLength)
//     row count (uint64)
//     for each column, per its Kind:
//       validity: word count (uint64) + that many uint64 words
//       data:
//         Int64  → row-count int64 values
//         Double → row-count double values
//         String → for each row: len (uint64) + bytes
//
// The typed column buffers are written verbatim, so save/load is a direct
// serialization of the in-memory columnar layout — no per-value boxing.
//
// The format is versioned: loading a newer major version fails with a clear
// error rather than misreading. This is an in-memory engine that snapshots to
// disk (not a mutable on-disk store); a load replaces the catalog's contents.
// ============================================================================

#include "catalog.h"

#include <stdexcept>
#include <string>

namespace lq {

struct PersistenceError : std::runtime_error {
    explicit PersistenceError(const std::string& msg) : std::runtime_error(msg) {}
};

// Current on-disk format version. Bump the major part on incompatible changes.
constexpr uint32_t kPersistenceVersion = 1;

// Write every table in `catalog` to `path`. Throws PersistenceError on I/O
// failure. Overwrites an existing file.
void saveDatabase(const Catalog& catalog, const std::string& path);

// Load a database file into `catalog`, adding its tables. Throws
// PersistenceError on I/O failure, a bad magic/version, or a corrupt file.
// Existing tables with the same name are replaced.
void loadDatabase(Catalog& catalog, const std::string& path);

}  // namespace lq
