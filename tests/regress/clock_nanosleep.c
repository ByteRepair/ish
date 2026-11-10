// clock_nanosleep, which Rust std's thread::sleep calls on Linux, relative
// and TIMER_ABSTIME; and the clocks a guest reads besides the two iSH
// started with, the coarse, raw and boot-time ones.
#include "sys.h"

enum {
    CLOCK_REALTIME = 0, CLOCK_MONOTONIC = 1, CLOCK_MONOTONIC_RAW = 4,
    CLOCK_REALTIME_COARSE = 5, CLOCK_MONOTONIC_COARSE = 6, CLOCK_BOOTTIME = 7,
    TIMER_ABSTIME = 1,
};

static slong sleep_(int clock, int flags, struct timespec_ *t) {
    return sys4(NR_clock_nanosleep, clock, flags, t, 0);
}

void _start(void) {
    static const int clocks[] = {
        CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW,
        CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME,
    };
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        struct timespec_ t = {0, 0};
        check(sys2(NR_clock_gettime, clocks[i], &t) == 0, "a clock could not be read");
        check(t.sec > 0, "a clock read zero");
    }

    struct timespec_ start = now_(CLOCK_MONOTONIC);
    struct timespec_ ten = {0, 10 * 1000000};
    check(sleep_(CLOCK_MONOTONIC, 0, &ten) == 0, "a relative sleep failed");
    check(!before_(now_(CLOCK_MONOTONIC), plus_ms_(start, 10)), "the relative sleep ended early");

    struct timespec_ deadline = plus_ms_(now_(CLOCK_MONOTONIC), 10);
    check(sleep_(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline) == 0, "an absolute sleep failed");
    check(!before_(now_(CLOCK_MONOTONIC), deadline), "the absolute sleep ended before its deadline");
    check(sleep_(CLOCK_MONOTONIC, TIMER_ABSTIME, &start) == 0, "an absolute sleep already past failed");

    deadline = plus_ms_(now_(CLOCK_REALTIME), 10);
    check(sleep_(CLOCK_REALTIME, TIMER_ABSTIME, &deadline) == 0, "a realtime absolute sleep failed");
    check(!before_(now_(CLOCK_REALTIME), deadline), "the realtime sleep ended before its deadline");

    struct timespec_ bad = {0, 1000000000};
    check(sleep_(CLOCK_MONOTONIC, 0, &bad) == -EINVAL, "a second's worth of nanoseconds was not EINVAL");
    check(sleep_(99, 0, &ten) == -EINVAL, "an unknown clock was not EINVAL");
    finish(1, 0);
}
