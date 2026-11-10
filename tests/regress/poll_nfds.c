// poll refuses more descriptors than RLIMIT_NOFILE with EINVAL, as Linux
// does, rather than sizing a host stack array by whatever the guest asks.
#include "sys.h"

void _start(void) {
    check(sys3(NR_poll, 0, 1 << 20, 0) == -EINVAL, "poll of a million descriptors was not EINVAL");
    check(sys3(NR_poll, 0, 0, 0) == 0, "poll of nothing did not return 0");
    finish(1, 0);
}
