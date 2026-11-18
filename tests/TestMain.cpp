#include "TestFramework.h"

#include <cstdlib>
#include <new>

namespace {
    thread_local bool countingAllocations = false;
    thread_local long allocationCount = 0;
}

void cktest::AllocationCounter::start() { allocationCount = 0; countingAllocations = true; }
long cktest::AllocationCounter::stop()  { countingAllocations = false; return allocationCount; }

void* operator new (std::size_t size)
{
    if (countingAllocations)
        ++allocationCount;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void operator delete (void* p) noexcept                { std::free (p); }
void operator delete (void* p, std::size_t) noexcept   { std::free (p); }

int main()
{
    int failedTests = 0;

    for (const auto& test : cktest::registry())
    {
        const int before = cktest::failureCount();
        test.body();
        const bool passed = cktest::failureCount() == before;
        std::printf ("%s  %s\n", passed ? "pass" : "FAIL", test.name);
        if (! passed)
            ++failedTests;
    }

    std::printf ("\n%zu tests, %d failed\n", cktest::registry().size(), failedTests);
    return failedTests == 0 ? 0 : 1;
}
