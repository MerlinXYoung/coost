#pragma once
#include <iostream>
#include <vector>

#include "co/color.h"
#include "co/fastring.h"
#include "co/flag.h"

namespace unitest {

// return number of failed test cases
__coapi int run_tests();

// deprecated, use run_tests() instead
inline int run_all_tests() { return run_tests(); }

namespace xx {

struct Failed {
    Failed(const char* c, const char* file, int line, fastring&& msg)
        : c(c), file(file), line(line), msg(std::move(msg)) {}
    const char* c;  // case name
    const char* file;
    int line;
    fastring msg;
};

struct Test {
    Test(const char* name, bool& e, void (*f)(Test&)) noexcept
        : name(name), c("default"), enabled(e), f(f) {}
    const char* name;  // test name
    const char* c;     // current case name
    bool& enabled;     // if this test unit is enabled
    void (*f)(Test&);
    std::vector<Failed> failed;
};

__coapi bool add_test(const char* name, bool& e, void (*f)(Test&));

}  // namespace xx
}  // namespace unitest

// define a test unit
#define DEF_test(_name_)                                                                           \
    DEF_bool(_name_, false, "enable this test if true");                                           \
    void _co_ut_##_name_(unitest::xx::Test&);                                                      \
    static bool _co_ut_v_##_name_ = unitest::xx::add_test(#_name_, FLG_##_name_, _co_ut_##_name_); \
    void _co_ut_##_name_(unitest::xx::Test& _t_)

// define a test case in the current unit
#define DEF_case(name) \
    _t_.c = #name;     \
    std::cout << " case " << #name << ':' << std::endl;

#define EXPECT(x)                                                                                  \
    {                                                                                              \
        if (x) {                                                                                   \
            std::cout << co::color::Color::green << "  EXPECT(" << #x << ") passed"                \
                      << co::color::Color::deflt << std::endl;                                     \
        } else {                                                                                   \
            fastring _U_s(32);                                                                     \
            _U_s << "EXPECT(" << #x << ") failed";                                                 \
            std::cout << co::color::Color::red << "  " << _U_s << co::color::Color::deflt          \
                      << std::endl;                                                                \
            _t_.failed.push_back(unitest::xx::Failed(_t_.c, __FILE__, __LINE__, std::move(_U_s))); \
        }                                                                                          \
    }

#define EXPECT_OP(x, y, op, opname)                                                                \
    {                                                                                              \
        auto _U_x = (x);                                                                           \
        auto _U_y = (y);                                                                           \
        if (_U_x op _U_y) {                                                                        \
            std::cout << co::color::Color::green << "  EXPECT_" << opname << "(" << #x << ", "     \
                      << #y << ") passed";                                                         \
            if (strcmp("==", #op) != 0) std::cout << ": " << _U_x << " vs " << _U_y;               \
            std::cout << co::color::Color::deflt << std::endl;                                     \
        } else {                                                                                   \
            fastring _U_s(128);                                                                    \
            _U_s << "EXPECT_" << opname << "(" << #x << ", " << #y << ") failed: " << _U_x         \
                 << " vs " << _U_y;                                                                \
            std::cout << co::color::Color::red << "  " << _U_s << co::color::Color::deflt          \
                      << std::endl;                                                                \
            _t_.failed.push_back(unitest::xx::Failed(_t_.c, __FILE__, __LINE__, std::move(_U_s))); \
        }                                                                                          \
    }

#define EXPECT_EQ(x, y) EXPECT_OP(x, y, ==, "EQ")
#define EXPECT_NE(x, y) EXPECT_OP(x, y, !=, "NE")
#define EXPECT_GE(x, y) EXPECT_OP(x, y, >=, "GE")
#define EXPECT_LE(x, y) EXPECT_OP(x, y, <=, "LE")
#define EXPECT_GT(x, y) EXPECT_OP(x, y, >, "GT")
#define EXPECT_LT(x, y) EXPECT_OP(x, y, <, "LT")
