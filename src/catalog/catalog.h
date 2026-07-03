#pragma once

// ============================================================================
// LiteQuery — catalog.h
// The table registry: names → (schema, data).
//
// The Catalog is the single source of truth for what tables exist, their
// schemas, and (in this in-memory engine) the actual column data. The planner
// and optimizer only ever consult schemas and allocate node ids through it; the
// executor asks it for the concrete Table to scan.
//
// Thread-safety: reads are guarded by a shared_mutex so many Connections can
// plan concurrently while writes (CREATE/DROP/INSERT) take an exclusive lock.
// ============================================================================

#include "types.h"
#include "table.h"

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace lq {

class Catalog {
public:
    // ---- Registration ------------------------------------------------------

    // Register a table by schema only (data added later via getTable()->insertRow).
    void registerTable(const std::string& name, Schema schema) {
        std::unique_lock lock(mutex_);
        tables_[name] = std::make_shared<Table>(name, std::move(schema));
    }

    // Register a fully-populated table.
    void registerTable(TablePtr table) {
        std::unique_lock lock(mutex_);
        const std::string name = table->name();
        tables_[name] = std::move(table);
    }

    bool dropTable(const std::string& name) {
        std::unique_lock lock(mutex_);
        return tables_.erase(name) > 0;
    }

    // ---- Lookup ------------------------------------------------------------

    const Schema& getSchema(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = tables_.find(name);
        if (it == tables_.end())
            throw std::runtime_error("Unknown table: " + name);
        return it->second->schema();
    }

    TablePtr getTable(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = tables_.find(name);
        if (it == tables_.end())
            throw std::runtime_error("Unknown table: " + name);
        return it->second;
    }

    bool hasTable(const std::string& name) const {
        std::shared_lock lock(mutex_);
        return tables_.count(name) > 0;
    }

    std::vector<std::string> tableNames() const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> names;
        names.reserve(tables_.size());
        for (const auto& [n, _] : tables_) names.push_back(n);
        return names;
    }

    // Monotonically increasing id for plan nodes (mutable: logically const).
    uint32_t allocNodeId() const noexcept { return ++nextNodeId_; }

private:
    mutable std::shared_mutex                    mutex_;
    std::unordered_map<std::string, TablePtr>    tables_;
    mutable uint32_t                             nextNodeId_ = 0;
};

}  // namespace lq
