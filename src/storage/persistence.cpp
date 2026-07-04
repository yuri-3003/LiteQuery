// ============================================================================
// LiteQuery — persistence.cpp
// Binary save/load of the catalog. See persistence.h for the format.
// ============================================================================

#include "persistence.h"
#include "table.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace lq {

namespace {

constexpr char kMagic[4] = {'L', 'Q', 'D', 'B'};

// ---- Little-endian primitive writers ---------------------------------------

struct Writer {
    std::ofstream& os;

    void bytes(const void* p, size_t n) { os.write(static_cast<const char*>(p), n); }

    void u32(uint32_t v) {
        char b[4];
        for (int i = 0; i < 4; ++i) b[i] = char((v >> (8 * i)) & 0xFF);
        bytes(b, 4);
    }
    void u64(uint64_t v) {
        char b[8];
        for (int i = 0; i < 8; ++i) b[i] = char((v >> (8 * i)) & 0xFF);
        bytes(b, 8);
    }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        u64(bits);
    }
    void str(const std::string& s) {
        u64(s.size());
        if (!s.empty()) bytes(s.data(), s.size());
    }
    void dataType(const DataType& t) {
        u32(static_cast<uint32_t>(t.id));
        os.put(t.nullable ? 1 : 0);
        os.put(static_cast<char>(t.scale));
        os.put(static_cast<char>(t.precision));
        u32(static_cast<uint32_t>(t.maxLength));
    }
};

// ---- Little-endian primitive readers ---------------------------------------

struct Reader {
    std::ifstream& is;

    void bytes(void* p, size_t n) {
        is.read(static_cast<char*>(p), n);
        if (static_cast<size_t>(is.gcount()) != n)
            throw PersistenceError("unexpected end of file");
    }
    uint32_t u32() {
        unsigned char b[4];
        bytes(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
               (uint32_t(b[3]) << 24);
    }
    uint64_t u64() {
        unsigned char b[8];
        bytes(b, 8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
        return v;
    }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    double f64() {
        uint64_t bits = u64();
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }
    std::string str() {
        uint64_t n = u64();
        std::string s(n, '\0');
        if (n) bytes(s.data(), n);
        return s;
    }
    DataType dataType() {
        DataType t;
        t.id = static_cast<TypeId>(u32());
        char nb = 0; is.get(nb); t.nullable = nb != 0;
        char sc = 0; is.get(sc); t.scale = static_cast<uint8_t>(sc);
        char pr = 0; is.get(pr); t.precision = static_cast<uint8_t>(pr);
        t.maxLength = static_cast<int32_t>(u32());
        if (!is) throw PersistenceError("truncated DataType");
        return t;
    }
};

}  // namespace

// ============================================================================
// saveDatabase
// ============================================================================

void saveDatabase(const Catalog& catalog, const std::string& path) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) throw PersistenceError("cannot open file for writing: " + path);
    Writer w{os};

    w.bytes(kMagic, 4);
    w.u32(kPersistenceVersion);

    auto names = catalog.tableNames();
    w.u64(names.size());

    for (const auto& name : names) {
        TablePtr t = catalog.getTable(name);
        const Schema& schema = t->schema();

        w.str(t->name());
        w.u64(schema.size());
        for (size_t c = 0; c < schema.size(); ++c) {
            w.str(schema[c].name);
            w.dataType(schema[c].type);
        }

        const uint64_t rows = t->rowCount();
        w.u64(rows);

        for (size_t c = 0; c < t->columnCount(); ++c) {
            const Column& col = t->columnAt(c);

            // Validity bitmap words.
            const auto& words = col.validity().words();
            w.u64(words.size());
            for (uint64_t word : words) w.u64(word);

            // Typed data.
            switch (col.kind()) {
                case Column::Kind::Int64:
                    for (int64_t v : col.i64()) w.i64(v);
                    break;
                case Column::Kind::Double:
                    for (double v : col.f64()) w.f64(v);
                    break;
                case Column::Kind::String:
                    for (const auto& s : col.str()) w.str(s);
                    break;
            }
        }
    }

    if (!os) throw PersistenceError("error while writing: " + path);
}

// ============================================================================
// loadDatabase
// ============================================================================

void loadDatabase(Catalog& catalog, const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) throw PersistenceError("cannot open file for reading: " + path);
    Reader r{is};

    char magic[4];
    r.bytes(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0)
        throw PersistenceError("not a LiteQuery database file (bad magic): " + path);

    uint32_t version = r.u32();
    if (version > kPersistenceVersion)
        throw PersistenceError(
            "database file version " + std::to_string(version) +
            " is newer than this build supports (" +
            std::to_string(kPersistenceVersion) + ")");

    uint64_t tableCount = r.u64();
    for (uint64_t ti = 0; ti < tableCount; ++ti) {
        std::string tableName = r.str();

        Schema schema;
        uint64_t ncol = r.u64();
        for (uint64_t c = 0; c < ncol; ++c) {
            std::string colName = r.str();
            DataType dt = r.dataType();
            schema.addColumn(colName, dt);
        }

        uint64_t rows = r.u64();
        auto table = std::make_shared<Table>(tableName, schema);

        for (uint64_t c = 0; c < ncol; ++c) {
            Column& col = table->columnAt(c);

            uint64_t wordCount = r.u64();
            std::vector<uint64_t> valid(wordCount);
            for (uint64_t w = 0; w < wordCount; ++w) valid[w] = r.u64();

            switch (col.kind()) {
                case Column::Kind::Int64: {
                    std::vector<int64_t> data(rows);
                    for (uint64_t i = 0; i < rows; ++i) data[i] = r.i64();
                    col.restoreInt64(std::move(data), std::move(valid), rows);
                    break;
                }
                case Column::Kind::Double: {
                    std::vector<double> data(rows);
                    for (uint64_t i = 0; i < rows; ++i) data[i] = r.f64();
                    col.restoreDouble(std::move(data), std::move(valid), rows);
                    break;
                }
                case Column::Kind::String: {
                    std::vector<std::string> data(rows);
                    for (uint64_t i = 0; i < rows; ++i) data[i] = r.str();
                    col.restoreString(std::move(data), std::move(valid), rows);
                    break;
                }
            }
        }

        catalog.registerTable(std::move(table));
    }
}

}  // namespace lq
