// Installs a SIGUSR1 handler, sends itself SIGUSR1, and checks the handler
// ran and the process resumed. The handler is a normal C function; the kernel
// pushes pretcode (the SA_RESTORER) as the return address, so its `ret`
// enters the restorer, which makes the rt_sigreturn syscall. Exercises the
// x86-64 signal frame in both directions.
#include "sys.h"

#define SIGUSR1 10
#define SA_RESTORER 0x04000000

#if defined(__x86_64__)
enum { NR_rt_sigaction = 13, NR_rt_sigreturn = 15, NR_kill = 62, NR_getpid = 39 };
struct sigaction64 { ulong handler, flags, restorer, mask; };
static volatile int hit = 0;
static void handler(void) { hit = 1; }
__attribute__((naked)) static void restorer(void) {
    __asm__("mov $15, %rax\n\tsyscall");
}
void _start(void) {
    struct sigaction64 sa = {
        .handler = (ulong) (ulong) &handler,
        .flags = SA_RESTORER,
        .restorer = (ulong) (ulong) &restorer,
        .mask = 0,
    };
    check(sys4(NR_rt_sigaction, SIGUSR1, &sa, 0, 8) == 0, "sigaction failed");
    check(sys2(NR_kill, sys0(NR_getpid), SIGUSR1) == 0, "kill failed");
    check(hit == 1, "handler did not run");
    finish(1, "");
}
#elif defined(__i386__)
enum { NR_rt_sigaction = 174, NR_rt_sigreturn = 173, NR_kill = 37, NR_getpid = 20 };
struct sigaction32 { ulong handler, flags, restorer, mask; };
static volatile int hit = 0;
static void handler(void) { hit = 1; }
void _start(void) {
    struct sigaction32 sa = {
        .handler = (ulong) &handler, .flags = 0, .restorer = 0, .mask = 0,
    };
    check(sys4(NR_rt_sigaction, SIGUSR1, &sa, 0, 8) == 0, "sigaction failed");
    check(sys2(NR_kill, sys0(NR_getpid), SIGUSR1) == 0, "kill failed");
    check(hit == 1, "handler did not run");
    finish(1, "");
}
#endif
