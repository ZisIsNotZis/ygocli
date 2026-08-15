// tests/test.h — minimal test harness (zero dependencies).
//
// CHECK/CHECK_EQ record failures and continue; the runner reports and exits 1
// on any failure. TEST(name) registers a function into the global registry.
// No exceptions, no macros beyond this — keep the test surface dumb.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline int& Failures() {
    static int n = 0;
    return n;
}

inline const char*& CurrentTest() {
    static const char* t = "";
    return t;
}

inline void Report(bool ok, const char* expr, const char* file, int line, const std::string& detail = "") {
    if (!ok) {
        std::fprintf(stderr, "  FAIL %s:%d: %s%s%s\n", file, line, expr,
                     detail.empty() ? "" : " [", detail.c_str());
        if (!detail.empty()) std::fprintf(stderr, "]\n");
        ++Failures();
    }
}

inline std::string Str(int64_t v) { return std::to_string(v); }
inline std::string Str(uint64_t v) { return std::to_string(v); }
inline std::string Str(const std::string& v) { return v; }
inline std::string Str(const char* v) { return v ? std::string(v) : "(null)"; }
template <typename T>
std::string Str(const T& v) { return std::to_string(static_cast<int64_t>(v)); }

}  // namespace test

#define TEST(name)                                                        \
    static void test_fn_##name();                                         \
    static ::test::Registrar test_reg_##name(#name, test_fn_##name);      \
    static void test_fn_##name()

#define CHECK(cond)                                                        \
    do {                                                                   \
        bool ok_ = (cond);                                                 \
        ::test::Report(ok_, #cond, __FILE__, __LINE__);                    \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto va_ = (a);                                                    \
        auto vb_ = (b);                                                    \
        bool ok_ = (va_ == vb_);                                           \
        std::string det_;                                                  \
        if (!ok_)                                                          \
            det_ = ::test::Str(va_) + " != " + ::test::Str(vb_);           \
        ::test::Report(ok_, #a " == " #b, __FILE__, __LINE__, det_);       \
    } while (0)

#define CHECK_MSG(cond, msg)                                               \
    do {                                                                   \
        bool ok_ = (cond);                                                 \
        ::test::Report(ok_, #cond, __FILE__, __LINE__, msg);               \
    } while (0)
