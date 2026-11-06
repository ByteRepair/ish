// clone's x86-64 ABI passes (flags, stack, ptid, ctid, tls): the child-tid
// and TLS arguments are swapped relative to i386's order. The kernel's
// sys_clone64 reorders them before the shared sys_clone. This test clones
// with CLONE_SETTLS and CLONE_CHILD_SETTID and checks that the FS base is
// the TLS value and the child pid lands in the child-tid slot, which fail if
// the two are swapped. i386 has a different ABI and TLS (a user_desc), so
// there it only checks a basic clone returns the child's pid.
#include "sys.h"

// clone flags
#define CSIGNAL 0x000000ff
#define CLONE_VM 0x00000100
#define CLONE_SETTLS 0x00080000
#define CLONE_CHILD_SETTID 0x01000000
#define SIGCHLD 17

#if defined(__x86_64__)
enum { NR_clone = 56, NR_arch_prctl = 158, NR_getpid = 39 };
#define ARCH_GET_FS 0x1003

// clone(flags, stack, ptid, ctid, tls) on x86-64
static inline slong clone_(slong flags, void *stack, int *ptid, int *ctid, void *tls) {
    return sys6(NR_clone, flags, (slong) stack, (slong) ptid, (slong) ctid, (slong) tls, 0);
}

static const char marker = 0;

static void child(void) {
    slong fs = 0;
    sys2(NR_arch_prctl, ARCH_GET_FS, &fs);
    check(fs == 0x1111222233334444L, "FS base is not the TLS value");
    sys1(NR_exit_group, 0);
    __builtin_unreachable();
}

void _start(void) {
    static char stack[4096];
    static int ctid = 0;
    void *tls = (void *) 0x1111222233334444L;
    slong pid = clone_(CLONE_VM | CLONE_SETTLS | CLONE_CHILD_SETTID | SIGCHLD,
                       stack + sizeof(stack), 0, &ctid, tls);
    check(pid >= 0, "clone failed");
    if (pid == 0)
        child();
    int status = 0;
    check(waitpid_(pid, &status) == pid, "waitpid failed");
    check(status == 0, "child misbehaved");
    check(ctid == (int) pid, "child pid not written to the child-tid slot");
    finish(1, "");
}

#elif defined(__i386__)
enum { NR_clone = 120, NR_getpid = 20 };

// clone(flags, stack, ptid, tls, ctid) on i386, all six slots
static inline slong clone_(slong flags, void *stack, int *ptid, void *tls, int *ctid) {
    return sys6(NR_clone, flags, (slong) stack, (slong) ptid, (slong) tls, (slong) ctid, 0);
}

void _start(void) {
    static char stack[4096];
    slong pid = clone_(CLONE_VM | SIGCHLD, stack + sizeof(stack), 0, 0, 0);
    check(pid >= 0, "clone failed");
    if (pid == 0)
        sys1(NR_exit_group, 0), __builtin_unreachable();
    int status = 0;
    check(waitpid_(pid, &status) == pid, "waitpid failed");
    finish(1, "");
}
#endif
