// FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET, which every Rust std lock and
// condvar waits through: an absolute timeout on CLOCK_MONOTONIC, or on
// CLOCK_REALTIME with FUTEX_CLOCK_REALTIME, and a wake that reaches only the
// waiters whose bitset it shares. Without them the wait answers ENOSYS at
// once and each blocked thread spins.
#include "sys.h"

enum {
    FUTEX_WAIT = 0, FUTEX_WAIT_BITSET = 9, FUTEX_WAKE_BITSET = 10,
    FUTEX_PRIVATE = 128, FUTEX_CLOCK_REALTIME = 256,
    CLOCK_REALTIME = 0, CLOCK_MONOTONIC = 1,
    CLONE_VM = 0x100, SIGCHLD = 17,
};

static int word = 1;

static slong wait_(int op, int val, struct timespec_ *timeout, int bitset) {
    return sys6(NR_futex, (slong) &word, op | FUTEX_PRIVATE, val, (slong) timeout, 0, bitset);
}

static slong wake_(int bitset) {
    return sys6(NR_futex, (slong) &word, FUTEX_WAKE_BITSET | FUTEX_PRIVATE, 1, 0, 0, bitset);
}

static slong clone_vm(void *stack) {
    // the child-tid and TLS slots are swapped between the two ABIs, and
    // both are 0 here
    return sys6(NR_clone, CLONE_VM | SIGCHLD, (slong) stack, 0, 0, 0, 0);
}

static __attribute__((noreturn)) void waiter(void) {
    slong r = wait_(FUTEX_WAIT_BITSET, 1, 0, 1);
    sys1(NR_exit_group, r == 0 ? 0 : 1);
    __builtin_unreachable();
}

void _start(void) {
    check(wait_(FUTEX_WAIT_BITSET, 0, 0, -1) == -EAGAIN, "a wait on a changed word did not say EAGAIN");
    check(wait_(FUTEX_WAIT_BITSET, 1, 0, 0) == -EINVAL, "an empty bitset was not EINVAL");

    struct timespec_ start = now_(CLOCK_MONOTONIC);
    struct timespec_ deadline = plus_ms_(start, 20);
    check(wait_(FUTEX_WAIT_BITSET, 1, &deadline, -1) == -ETIMEDOUT, "a monotonic deadline did not time out");
    check(!before_(now_(CLOCK_MONOTONIC), deadline), "the wait ended before its monotonic deadline");
    check(wait_(FUTEX_WAIT_BITSET, 1, &start, -1) == -ETIMEDOUT, "a deadline already past did not time out");

    deadline = plus_ms_(now_(CLOCK_REALTIME), 20);
    check(wait_(FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 1, &deadline, -1) == -ETIMEDOUT,
          "a realtime deadline did not time out");
    check(!before_(now_(CLOCK_REALTIME), deadline), "the wait ended before its realtime deadline");

    // FUTEX_WAIT's timeout stays relative
    struct timespec_ relative = {0, 20 * 1000000};
    start = now_(CLOCK_MONOTONIC);
    check(wait_(FUTEX_WAIT, 1, &relative, 0) == -ETIMEDOUT, "a relative wait did not time out");
    check(!before_(now_(CLOCK_MONOTONIC), plus_ms_(start, 20)), "the relative wait ended early");

    check(wake_(-1) == 0, "a wake with nobody waiting woke someone");
    static char stack[16384];
    slong pid = clone_vm(stack + sizeof(stack));
    check(pid >= 0, "clone failed");
    if (pid == 0)
        waiter();
    // the waiter may not be queued yet; a wake that finds nobody says 0
    slong woken;
    while ((woken = wake_(1)) == 0)
        sys0(NR_sched_yield);
    check(woken == 1, "the bitset wake did not wake exactly the waiter");
    int status = -1;
    check(waitpid_(pid, &status) == pid, "waitpid failed");
    check(status == 0, "the waiter's wait did not return 0");
    finish(1, 0);
}
