// The tiny test runner shared by the sample library test programs. No framework.
//
// Each TEST registers a function. PHYLO_TEST_MAIN runs them all, prints failures, and
// returns non-zero if any check failed, so ctest picks it up. Include this in exactly one
// source file per test program.
#pragma once

#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

namespace phylo_test
{

struct TestCase
{
    const char* name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int gFailures = 0;
inline const char* gCurrent = "";

struct Registrar
{
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline int runAll()
{
    int ran = 0;
    for (const auto& t : registry())
    {
        gCurrent = t.name;
        const int before = gFailures;
        t.body();
        ++ran;
        std::printf("%s %s\n", gFailures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("\n%d tests, %d failed checks\n", ran, gFailures);
    return gFailures == 0 ? 0 : 1;
}

} // namespace phylo_test

#define TEST(name)                                                      \
    static void name();                                                 \
    static phylo_test::Registrar name##_registrar(#name, &name);        \
    static void name()

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, phylo_test::gCurrent, #cond);  \
            ++phylo_test::gFailures;                                                                 \
        }                                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        const auto va = (a);                                                                         \
        const auto vb = (b);                                                                         \
        if (!(va == vb)) {                                                                           \
            std::printf("  FAIL %s:%d [%s] %s == %s (got %lld, want %lld)\n", __FILE__, __LINE__,    \
                        phylo_test::gCurrent, #a, #b, static_cast<long long>(va),                    \
                        static_cast<long long>(vb));                                                 \
            ++phylo_test::gFailures;                                                                 \
        }                                                                                            \
    } while (0)

#define PHYLO_TEST_MAIN                                                 \
    int main() { return phylo_test::runAll(); }
