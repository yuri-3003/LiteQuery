#pragma once

// ============================================================================
// LiteQuery — test_framework.h
// A tiny, zero-dependency test harness (keeps LiteQuery dependency-free).
//
//   TEST(name) { ... CHECK(cond); CHECK_EQ(a, b); ... }
//   int main() { return lqtest::run_all(); }
//
// Tests self-register via a static initializer. CHECK failures are recorded and
// reported per test; the process exit code is non-zero if any test fails.
// ============================================================================

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace lqtest {

struct Case {
    std::string           name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

// Per-test failure counter (reset before each test runs).
inline int& currentFailures() { static int f = 0; return f; }

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int run_all() {
    int passed = 0, failed = 0;
    for (auto& c : registry()) {
        currentFailures() = 0;
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::printf("  [throw] %s: %s\n", c.name.c_str(), e.what());
            currentFailures()++;
        } catch (...) {
            std::printf("  [throw] %s: unknown exception\n", c.name.c_str());
            currentFailures()++;
        }
        if (currentFailures() == 0) {
            std::printf("  [PASS] %s\n", c.name.c_str());
            ++passed;
        } else {
            std::printf("  [FAIL] %s (%d checks failed)\n", c.name.c_str(), currentFailures());
            ++failed;
        }
    }
    std::printf("\n%d passed, %d failed, %d total\n", passed, failed, passed + failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace lqtest

#define TEST(name)                                                            \
    static void name();                                                       \
    static ::lqtest::Registrar reg_##name(#name, name);                       \
    static void name()

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("    CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ::lqtest::currentFailures()++;                                    \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        auto _va = (a); auto _vb = (b);                                       \
        if (!(_va == _vb)) {                                                  \
            std::ostringstream _os;                                           \
            _os << "    CHECK_EQ failed: " << #a << " == " << #b              \
                << "  (" << _va << " vs " << _vb << ")  ("                    \
                << __FILE__ << ":" << __LINE__ << ")";                        \
            std::printf("%s\n", _os.str().c_str());                           \
            ::lqtest::currentFailures()++;                                    \
        }                                                                     \
    } while (0)
