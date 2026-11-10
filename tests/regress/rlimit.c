// setrlimit then getrlimit gives back both limits, the soft and the hard,
// in the guest's own struct rlimit: two longs.
#include "sys.h"

enum { RLIMIT_NOFILE = 7 };
struct rlimit { ulong cur, max; };

void _start(void) {
    struct rlimit set = {100, 200}, got = {0, 0};
    check(sys2(NR_setrlimit, RLIMIT_NOFILE, &set) == 0, "setrlimit failed");
    check(sys2(NR_getrlimit, RLIMIT_NOFILE, &got) == 0, "getrlimit failed");
    check(got.cur == 100, "the soft limit did not come back");
    check(got.max == 200, "the hard limit did not come back");
    finish(1, 0);
}
