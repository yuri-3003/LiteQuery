// ============================================================================
// LiteQuery — lq (interactive SQL shell)
//
// A small REPL over lq::Connection. Type SQL, see results. Statements are
// terminated by ';' and may span multiple lines. Dot-commands (.help, .tables,
// .schema, .mode, .read, .quit) control the shell itself.
//
// Non-interactive use:
//   lq                       start the REPL
//   lq script.sql            run a .sql file, then exit
//   echo "SELECT 1" | lq     run SQL piped on stdin, then exit
//   lq -c "SELECT 1+1"       run one statement and exit
//
// Output formats (.mode): table (default) | csv | json | list
// ============================================================================

#include "connection.h"
#include "litequery/litequery.h"   // LITEQUERY_VERSION_STRING

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <io.h>
  #define LQ_ISATTY(fd) _isatty(fd)
  #define LQ_FILENO(f)  _fileno(f)
#else
  #include <unistd.h>
  #define LQ_ISATTY(fd) isatty(fd)
  #define LQ_FILENO(f)  fileno(f)
#endif

using namespace lq;

namespace {

// ---- ANSI colours (disabled when output isn't a TTY or NO_COLOR is set) ----
struct Style {
    bool on = false;
    const char* dim()    const { return on ? "\x1b[2m"  : ""; }
    const char* bold()   const { return on ? "\x1b[1m"  : ""; }
    const char* accent() const { return on ? "\x1b[33m" : ""; }  // amber
    const char* red()    const { return on ? "\x1b[31m" : ""; }
    const char* green()  const { return on ? "\x1b[32m" : ""; }
    const char* reset()  const { return on ? "\x1b[0m"  : ""; }
};

enum class Mode { Table, Csv, Json, List };

struct Shell {
    Connection conn;
    Style      style;
    Mode       mode = Mode::Table;
    bool       timing = true;
    bool       interactive = true;

    // ---- value → display string --------------------------------------------
    static std::string cell(const Value& v) {
        if (v.isNull()) return "NULL";
        if (v.typeId() == TypeId::VARCHAR) return v.getString();
        if (v.typeId() == TypeId::BOOLEAN) return v.getBool() ? "true" : "false";
        if (typeIsInteger(v.typeId()))     return std::to_string(v.toInt64());
        if (typeIsFloat(v.typeId())) {
            // Trim trailing zeros for readability.
            std::ostringstream os; os << v.toDouble();
            return os.str();
        }
        return v.toString();
    }

    static std::string jsonEscape(const std::string& s) {
        std::string o; o.reserve(s.size() + 2);
        for (char c : s) {
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\t': o += "\\t";  break;
                case '\r': o += "\\r";  break;
                default:   o += c;      break;
            }
        }
        return o;
    }

    static std::string csvField(const std::string& s) {
        bool needQuote = s.find_first_of(",\"\n\r") != std::string::npos;
        if (!needQuote) return s;
        std::string o = "\"";
        for (char c : s) { if (c == '"') o += '"'; o += c; }
        o += '"';
        return o;
    }

    // ---- render a result set -----------------------------------------------
    void printResult(const QueryResult& r) {
        if (r.error) {
            std::cerr << style.red() << "Error: " << style.reset()
                      << r.errorMessage << "\n";
            return;
        }
        // DDL/DML: no columns.
        if (r.schema.size() == 0) {
            if (interactive) {
                std::cout << style.green() << "OK" << style.reset();
                if (r.rowsAffected)
                    std::cout << style.dim() << "  (" << r.rowsAffected
                              << " row" << (r.rowsAffected == 1 ? "" : "s")
                              << " affected)" << style.reset();
                std::cout << "\n";
            }
            return;
        }

        switch (mode) {
            case Mode::Table: printTable(r); break;
            case Mode::Csv:   printCsv(r);   break;
            case Mode::Json:  printJson(r);  break;
            case Mode::List:  printList(r);  break;
        }

        if (interactive && timing) {
            std::cout << style.dim() << r.rows.size() << " row"
                      << (r.rows.size() == 1 ? "" : "s")
                      << "  ·  " << r.elapsedMicros << " \xC2\xB5s"
                      << style.reset() << "\n";
        }
    }

    void printTable(const QueryResult& r) {
        const size_t nc = r.schema.size();
        std::vector<size_t> w(nc, 0);
        std::vector<std::string> headers(nc);
        for (size_t c = 0; c < nc; ++c) {
            headers[c] = r.schema[c].name;
            w[c] = headers[c].size();
        }
        std::vector<std::vector<std::string>> body(r.rows.size(), std::vector<std::string>(nc));
        for (size_t i = 0; i < r.rows.size(); ++i)
            for (size_t c = 0; c < nc; ++c) {
                body[i][c] = cell(r.rows[i][c]);
                w[c] = std::max(w[c], body[i][c].size());
            }

        auto rule = [&](const char* l, const char* m, const char* rt) {
            std::cout << style.dim() << l;
            for (size_t c = 0; c < nc; ++c) {
                for (size_t k = 0; k < w[c] + 2; ++k) std::cout << "\xE2\x94\x80"; // ─
                std::cout << (c + 1 < nc ? m : rt);
            }
            std::cout << style.reset() << "\n";
        };
        auto row = [&](const std::vector<std::string>& cells, bool head) {
            std::cout << style.dim() << "\xE2\x94\x82" << style.reset(); // │
            for (size_t c = 0; c < nc; ++c) {
                std::string pad(w[c] - cells[c].size(), ' ');
                std::cout << ' ';
                if (head) std::cout << style.bold() << style.accent();
                std::cout << cells[c];
                if (head) std::cout << style.reset();
                std::cout << pad << ' ' << style.dim() << "\xE2\x94\x82" << style.reset();
            }
            std::cout << "\n";
        };

        rule("\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90");   // ┌ ┬ ┐
        row(headers, true);
        rule("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4");   // ├ ┼ ┤
        for (const auto& b : body) row(b, false);
        rule("\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98");   // └ ┴ ┘
    }

    void printCsv(const QueryResult& r) {
        const size_t nc = r.schema.size();
        for (size_t c = 0; c < nc; ++c)
            std::cout << (c ? "," : "") << csvField(r.schema[c].name);
        std::cout << "\n";
        for (const auto& row : r.rows) {
            for (size_t c = 0; c < nc; ++c)
                std::cout << (c ? "," : "") << csvField(cell(row[c]));
            std::cout << "\n";
        }
    }

    void printJson(const QueryResult& r) {
        const size_t nc = r.schema.size();
        std::cout << "[";
        for (size_t i = 0; i < r.rows.size(); ++i) {
            std::cout << (i ? ",\n " : "\n ") << "{";
            for (size_t c = 0; c < nc; ++c) {
                const Value& v = r.rows[i][c];
                std::cout << (c ? ", " : "") << "\"" << jsonEscape(r.schema[c].name) << "\": ";
                if (v.isNull())                     std::cout << "null";
                else if (v.typeId() == TypeId::VARCHAR) std::cout << "\"" << jsonEscape(v.getString()) << "\"";
                else if (v.typeId() == TypeId::BOOLEAN) std::cout << (v.getBool() ? "true" : "false");
                else                                std::cout << cell(v);
            }
            std::cout << "}";
        }
        std::cout << (r.rows.empty() ? "]" : "\n]") << "\n";
    }

    void printList(const QueryResult& r) {
        const size_t nc = r.schema.size();
        for (const auto& row : r.rows) {
            for (size_t c = 0; c < nc; ++c)
                std::cout << (c ? "|" : "") << cell(row[c]);
            std::cout << "\n";
        }
    }

    // ---- dot-commands ------------------------------------------------------
    // Returns false if the shell should quit.
    bool dotCommand(const std::string& line) {
        std::istringstream is(line);
        std::string cmd; is >> cmd;

        if (cmd == ".quit" || cmd == ".exit" || cmd == ".q") return false;

        if (cmd == ".help") {
            std::cout <<
                ".help                 show this help\n"
                ".tables               list tables\n"
                ".schema [table]       show CREATE-like column list\n"
                ".import FILE TABLE     load a CSV file into a new table\n"
                ".import -t FILE TABLE  same, but tab-separated (TSV)\n"
                ".save FILE            save the database to a file\n"
                ".open FILE            load a database from a file\n"
                ".mode MODE            output mode: table | csv | json | list\n"
                ".timing on|off        toggle the timing footer\n"
                ".read FILE            execute SQL from a file\n"
                ".quit                 exit\n";
            return true;
        }
        if (cmd == ".save" || cmd == ".open") {
            std::string file; is >> file;
            if (file.empty()) { std::cerr << "usage: " << cmd << " FILE\n"; return true; }
            QueryResult r = (cmd == ".save") ? conn.saveDatabase(file)
                                             : conn.loadDatabase(file);
            if (r.error)
                std::cerr << style.red() << "Error: " << style.reset() << r.errorMessage << "\n";
            else
                std::cout << style.green() << "OK" << style.reset()
                          << style.dim() << "  (" << r.rowsAffected << " tables)"
                          << style.reset() << "\n";
            return true;
        }
        if (cmd == ".import") {
            // .import [-t] FILE TABLE     (-t → tab-separated)
            std::string a1, a2, a3;
            is >> a1 >> a2;
            char delim = ',';
            std::string file, table;
            if (a1 == "-t") { delim = '\t'; is >> a3; file = a2; table = a3; }
            else            { file = a1; table = a2; }
            if (file.empty() || table.empty()) {
                std::cerr << "usage: .import [-t] FILE TABLE\n";
                return true;
            }
            QueryResult r = conn.importCsv(file, table, delim, /*hasHeader=*/true);
            if (r.error) {
                std::cerr << style.red() << "Error: " << style.reset() << r.errorMessage << "\n";
            } else {
                std::cout << style.green() << "OK" << style.reset()
                          << style.dim() << "  (" << r.rowsAffected << " rows into "
                          << table << ")" << style.reset() << "\n";
            }
            return true;
        }
        if (cmd == ".tables") {
            auto names = conn.catalog().tableNames();
            std::sort(names.begin(), names.end());
            if (names.empty()) std::cout << style.dim() << "(no tables)" << style.reset() << "\n";
            for (const auto& n : names) std::cout << n << "\n";
            return true;
        }
        if (cmd == ".schema") {
            std::string tbl; is >> tbl;
            auto names = conn.catalog().tableNames();
            std::sort(names.begin(), names.end());
            for (const auto& n : names) {
                if (!tbl.empty() && n != tbl) continue;
                const Schema& s = conn.catalog().getSchema(n);
                std::cout << style.accent() << "CREATE TABLE " << n << style.reset() << " (\n";
                for (size_t i = 0; i < s.size(); ++i)
                    std::cout << "  " << s[i].name << " " << s[i].type.toString()
                              << (i + 1 < s.size() ? "," : "") << "\n";
                std::cout << ");\n";
            }
            return true;
        }
        if (cmd == ".mode") {
            std::string m; is >> m;
            if (m == "table") mode = Mode::Table;
            else if (m == "csv") mode = Mode::Csv;
            else if (m == "json") mode = Mode::Json;
            else if (m == "list") mode = Mode::List;
            else std::cerr << "unknown mode: " << m << " (table|csv|json|list)\n";
            return true;
        }
        if (cmd == ".timing") {
            std::string v; is >> v; timing = (v != "off");
            return true;
        }
        if (cmd == ".read") {
            std::string path; is >> path;
            runFile(path);
            return true;
        }
        std::cerr << "unknown command: " << cmd << "  (try .help)\n";
        return true;
    }

    // ---- execution ---------------------------------------------------------
    void runSql(const std::string& sql) {
        printResult(conn.query(sql));
    }

    // Execute a buffer that may contain several ';'-separated statements.
    void runBuffer(const std::string& buf) {
        std::string stmt;
        for (size_t i = 0; i < buf.size(); ++i) {
            char c = buf[i];
            stmt += c;
            if (c == ';') {
                std::string trimmed = trim(stmt);
                if (!trimmed.empty() && trimmed != ";") runSql(trimmed);
                stmt.clear();
            }
        }
        std::string tail = trim(stmt);
        if (!tail.empty()) runSql(tail);   // allow a trailing statement w/o ';'
    }

    // Shared statement buffer used by both the REPL and file/stdin execution,
    // so dot-commands and multi-line SQL behave identically everywhere.
    std::string pending_;

    // Feed one input line. Returns false if a .quit was seen.
    bool feedLine(const std::string& line) {
        std::string t = trim(line);

        // Dot-commands are recognized only at the start of a fresh statement.
        if (pending_.empty() && !t.empty() && t[0] == '.')
            return dotCommand(t);

        pending_ += line;
        pending_ += '\n';

        if (line.find(';') != std::string::npos) {
            runBuffer(pending_);
            pending_.clear();
        }
        return true;
    }

    // Flush any trailing statement with no closing ';'.
    void flushPending() {
        if (!trim(pending_).empty()) runBuffer(pending_);
        pending_.clear();
    }

    void runFile(const std::string& path) {
        std::ifstream f(path);
        if (!f) { std::cerr << "cannot open file: " << path << "\n"; return; }
        std::string line;
        while (std::getline(f, line))
            if (!feedLine(line)) break;
        flushPending();
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // ---- the REPL ----------------------------------------------------------
    int repl() {
        if (interactive) {
            std::cout << style.accent() << style.bold() << "LiteQuery" << style.reset()
                      << " " << style.dim() << "v" << lqVersion()
                      << "  ·  type .help for commands, .quit to exit"
                      << style.reset() << "\n";
        }

        std::string line;
        while (true) {
            if (interactive) {
                std::cout << style.accent()
                          << (pending_.empty() ? "lq> " : " ..> ")
                          << style.reset();
                std::cout.flush();
            }
            if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }
            if (!feedLine(line)) break;
        }
        flushPending();
        return 0;
    }

    static const char* lqVersion() { return LITEQUERY_VERSION_STRING; }
};

bool stdinIsTty() {
    return LQ_ISATTY(LQ_FILENO(stdin)) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    Shell sh;

    // Colour on only for an interactive TTY without NO_COLOR.
    bool tty = stdinIsTty();
    sh.style.on = tty && (std::getenv("NO_COLOR") == nullptr);
    sh.interactive = tty;

    // Argument handling.
    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-c" && i + 1 < args.size()) {          // one statement
            sh.interactive = false;
            sh.runBuffer(args[++i]);
            return 0;
        }
        if (a == "-h" || a == "--help") {
            std::cout <<
                "Usage: lq [options] [file.sql]\n"
                "  (no args)     start the interactive shell\n"
                "  file.sql      run a SQL file, then exit\n"
                "  -c \"SQL\"      run one statement, then exit\n"
                "  -h, --help    show this help\n"
                "Piped stdin (echo 'SELECT 1' | lq) is run non-interactively.\n";
            return 0;
        }
        // Otherwise treat as a .sql file to run.
        sh.interactive = false;
        sh.runFile(a);
        return 0;
    }

    // No file args: interactive REPL, or run piped stdin.
    return sh.repl();
}
