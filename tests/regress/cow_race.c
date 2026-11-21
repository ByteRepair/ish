// Threads writing the same copy-on-write pages at once after a fork, each its
// own slot of every page, which the test reads back: a page two threads both
// copied kept only one thread's writes.
#include "sys.h"

// CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD
#define THREAD_FLAGS 0x10f00

enum { THREADS = 6, PAGES = 64, ROUNDS = 40, STACK = 16384, STRIDE = 4096 / sizeof(long) };

static volatile int go;
static volatile int done;
static long *pages;
static char stacks[THREADS][STACK] __attribute__((aligned(16)));

static void worker(long t) {
    for (int round = 1; round <= ROUNDS; round++) {
        while (go < round)
            sys0(NR_sched_yield);
        for (int p = 0; p < PAGES; p++)
            pages[p * STRIDE + t] = round * 100 + t;
        __atomic_fetch_add(&done, 1, __ATOMIC_SEQ_CST);
    }
    sys1(NR_exit, 0);
}

// The child starts on its own stack with nothing to return to, so it calls
// worker from here.
#if defined(__x86_64__)
static slong spawn(long t) {
    slong ret;
    register long r12 __asm__("r12") = t;
    __asm__ volatile(
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"
        "mov %%r12, %%rdi\n\t"
        "call *%[fn]\n\t"
        "1:"
        : "=a"(ret)
        : "a"(NR_clone), "D"(THREAD_FLAGS), "S"(stacks[t] + STACK), "r"(r12), [fn] "r"(worker)
        : "rcx", "r11", "memory");
    return ret;
}
#elif defined(__i386__)
// i386's clone is (ebx flags, ecx stack, edx ptid, esi tls, edi ctid); with
// none of the tid or TLS flags edx is free for worker, and t rides on top of
// the child's stack as worker's argument.
static slong spawn(long t) {
    slong ret;
    long *top = (long *) (stacks[t] + STACK) - 4;
    top[0] = t;
    __asm__ volatile(
        "int $0x80\n\t"
        "test %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "call *%%edx\n\t"
        "1:"
        : "=a"(ret)
        : "a"(NR_clone), "b"(THREAD_FLAGS), "c"(top), "d"(worker)
        : "memory");
    return ret;
}
#endif

void _start(void) {
    pages = mmap_(0, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check((ulong) pages < (ulong) -4096, "mmap failed");
    // fault every page in, so the fork has something to share
    for (int p = 0; p < PAGES; p++)
        pages[p * STRIDE + THREADS] = 1;
    for (long t = 0; t < THREADS; t++)
        check(spawn(t) > 0, "clone failed");

    for (int round = 1; round <= ROUNDS; round++) {
        // every page shared with a child, then the child gone
        slong pid = sys0(NR_fork);
        if (pid == 0)
            sys1(NR_exit, 0);
        check(pid > 0, "fork failed");
        int status;
        check(waitpid_(pid, &status) == pid, "waitpid failed");

        go = round;
        while (done < round * THREADS)
            sys0(NR_sched_yield);
        for (int p = 0; p < PAGES; p++)
            for (int t = 0; t < THREADS; t++)
                check(pages[p * STRIDE + t] == round * 100 + t, "a thread's write to a copied page was lost");
    }
    finish(1, "");
}
