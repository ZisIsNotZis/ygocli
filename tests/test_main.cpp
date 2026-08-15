// tests/test_main.cpp — test runner.

#include <cstdio>

#include "test.h"

int main() {
    int failures = 0;
    for (const auto& c : test::Registry()) {
        const int before = test::Failures();
        c.fn();
        const bool ok = test::Failures() == before;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.name);
        if (!ok)
            ++failures;
    }
    const int total = static_cast<int>(test::Registry().size());
    std::printf("%d tests, %d failed\n", total, failures);
    return failures == 0 ? 0 : 1;
}
