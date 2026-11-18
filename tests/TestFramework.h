#pragma once

// A deliberately tiny test harness so the engine tests need nothing but a compiler.

#include <cstdio>
#include <functional>
#include <vector>

namespace cktest {

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

inline int& failureCount()
{
    static int failures = 0;
    return failures;
}

struct Registrar
{
    Registrar (const char* name, std::function<void()> body) { registry().push_back ({ name, std::move (body) }); }
};

/** Counts heap allocations on the current thread while enabled.
    Used to prove the audio path never allocates. */
struct AllocationCounter
{
    static void start();
    static long stop();
};

} // namespace cktest

#define CK_CONCAT_INNER(a, b) a##b
#define CK_CONCAT(a, b) CK_CONCAT_INNER (a, b)

#define CK_TEST(name)                                                                     \
    static void CK_CONCAT (ckTest_, __LINE__)();                                          \
    static cktest::Registrar CK_CONCAT (ckRegistrar_, __LINE__) (name, CK_CONCAT (ckTest_, __LINE__)); \
    static void CK_CONCAT (ckTest_, __LINE__)()

#define CK_CHECK(condition)                                                               \
    do {                                                                                  \
        if (! (condition)) {                                                              \
            std::printf ("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            ++cktest::failureCount();                                                     \
        }                                                                                 \
    } while (false)
