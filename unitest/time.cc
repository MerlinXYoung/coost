#include "co/time.h"

#include "co/def.h"
#include "co/str.h"
#include "co/unitest.h"

namespace test {

DEF_test(time) {
    DEF_case(mono) {
        int64_t us = now::us();
        int64_t ms = now::ms();
        EXPECT_GT(us, 0);
        EXPECT_GT(ms, 0);

        int64_t x = now::us();
        int64_t y = now::us();
        EXPECT_LE(x, y);
    }

    DEF_case(str) {
        fastring ymdhms = now::str("%Y%m%d%H%M%S");
        fastring ymd = now::str("%Y%m%d");
        EXPECT(ymdhms.starts_with(ymd));
    }

    DEF_case(sleep) {
        int64_t beg = now::ms();
        sleep::ms(1);
        int64_t end = now::ms();
        EXPECT_GE(end - beg, 1);
    }

    DEF_case(timer) {
        co::Timer timer;
        sleep::ms(1);
        int64_t t = timer.us();
        EXPECT_GE(t, 1000);
    }
}

}  // namespace test
