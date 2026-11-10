// Raw Linux syscalls for the regression guests: no libc, built on the host
// with clang -target <arch>-linux -nostdlib -static, so a test needs neither
// a compiler in the guest nor a network to install one.
#pragma once

typedef unsigned long ulong;
typedef long slong;

#if defined(__i386__)
static inline slong sys6(slong n, slong a, slong b, slong c, slong d, slong e, slong f) {
    slong ret;
    // ebp is the sixth argument and the frame pointer, so it is swapped in
    // around the trap rather than named as a constraint. It comes through
    // eax with the number: a memory operand may be esp-relative, and the
    // push would move it out from under the load.
    slong nf[2] = {n, f};
    __asm__ volatile("push %%ebp\n\tmov 4(%%eax), %%ebp\n\tmov (%%eax), %%eax\n\tint $0x80\n\tpop %%ebp"
                     : "=a"(ret)
                     : "a"(nf), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory");
    return ret;
}
enum {
    NR_exit = 1, NR_fork = 2, NR_read = 3, NR_write = 4, NR_open = 5,
    NR_close = 6, NR_waitpid = 7, NR_unlink = 10, NR_lseek = 19, NR_fcntl = 55,
    NR_old_mmap = 90, NR_munmap = 91, NR_nanosleep = 162, NR_mremap = 163,
    NR_setrlimit = 75, NR_poll = 168, NR_getrlimit = 191,
    NR_getrusage = 77, NR_statfs = 99, NR_writev = 146, NR_stat64 = 195,
    NR_fstat64 = 197, NR_clock_gettime = 265, NR_fstatat64 = 300,
    NR_pread64 = 180, NR_pwrite64 = 181, NR_ftruncate64 = 194,
    NR_fcntl64 = 221, NR_exit_group = 252, NR_futex = 240,
    NR_clock_nanosleep = 267, NR_clone = 120, NR_sched_yield = 158,
    NR_madvise = 219, NR_sysinfo = 116, NR_pipe = 42, NR_dup = 41,
    NR_epoll_create1 = 329, NR_epoll_ctl = 255, NR_epoll_wait = 256,
    NR_rt_sigprocmask = 175, NR_signalfd4 = 327,
};
#elif defined(__x86_64__)
static inline slong sys6(slong n, slong a, slong b, slong c, slong d, slong e, slong f) {
    slong ret;
    register slong r10 __asm__("r10") = d;
    register slong r8 __asm__("r8") = e;
    register slong r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
enum {
    NR_read = 0, NR_write = 1, NR_open = 2, NR_close = 3, NR_poll = 7,
    NR_lseek = 8, NR_mmap = 9, NR_munmap = 11, NR_pread64 = 17,
    NR_pwrite64 = 18, NR_mremap = 25, NR_nanosleep = 35, NR_fork = 57,
    NR_exit = 60, NR_wait4 = 61, NR_fcntl = 72, NR_ftruncate = 77,
    NR_unlink = 87, NR_getrlimit = 97, NR_setrlimit = 160, NR_exit_group = 231,
    NR_stat = 4, NR_fstat = 5, NR_writev = 20, NR_getrusage = 98,
    NR_statfs = 137, NR_clock_gettime = 228, NR_newfstatat = 262,
    NR_futex = 202, NR_clock_nanosleep = 230, NR_clone = 56,
    NR_sched_yield = 24, NR_madvise = 28, NR_sysinfo = 99, NR_pipe = 22,
    NR_dup = 32, NR_epoll_create1 = 291, NR_epoll_ctl = 233, NR_epoll_wait = 232,
    NR_preadv = 295, NR_preadv2 = 327, NR_rt_sigprocmask = 14,
    NR_signalfd4 = 289,
};
#else
#error "no syscall shim for this architecture yet"
#endif

#define sys0(n) sys6(n, 0, 0, 0, 0, 0, 0)
#define sys1(n, a) sys6(n, (slong)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b) sys6(n, (slong)(a), (slong)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c) sys6(n, (slong)(a), (slong)(b), (slong)(c), 0, 0, 0)
#define sys4(n, a, b, c, d) sys6(n, (slong)(a), (slong)(b), (slong)(c), (slong)(d), 0, 0)
#define sys5(n, a, b, c, d, e) sys6(n, (slong)(a), (slong)(b), (slong)(c), (slong)(d), (slong)(e), 0)

enum {
    PROT_READ = 1, PROT_WRITE = 2,
    MAP_PRIVATE = 2, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20,
    O_RDWR = 2, O_CREAT = 0100, O_TRUNC = 01000,
    F_SETLK = 6, F_SETLKW = 7, F_SETLK64 = 13, F_SETLKW64 = 14,
    F_WRLCK = 1, F_UNLCK = 2, SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2,
    MREMAP_MAYMOVE = 1,
    EAGAIN = 11, ENOMEM = 12, EFAULT = 14, EEXIST = 17, EINVAL = 22, ETIMEDOUT = 110,
};

static inline ulong strlen_(const char *s) {
    ulong n = 0;
    while (s[n]) n++;
    return n;
}

static inline void say(const char *s) {
    sys3(NR_write, 1, s, strlen_(s));
}

static inline __attribute__((noreturn)) void finish(int ok, const char *why) {
    say(ok ? "ok\n" : "FAIL: ");
    if (!ok) {
        say(why);
        say("\n");
    }
    sys1(NR_exit_group, ok ? 0 : 1);
    __builtin_unreachable();
}

#define check(cond, why) do { if (!(cond)) finish(0, why); } while (0)

static inline void *mmap_(void *addr, ulong len, int prot, int flags, int fd, ulong off) {
#if defined(__i386__)
    // old_mmap takes its six arguments as a struct, which keeps the shim
    // away from ebp for the one call that needs all six.
    ulong args[6] = {(ulong) addr, len, (ulong) prot, (ulong) flags, (ulong) fd, off};
    return (void *) sys1(NR_old_mmap, args);
#else
    return (void *) sys6(NR_mmap, (slong) addr, len, prot, flags, fd, off);
#endif
}

// The 64-bit offsets, which i386 passes as two halves, low first, and
// x86-64 whole.
static inline slong pread64_(slong fd, void *buf, ulong n, unsigned long long off) {
#if defined(__i386__)
    return sys5(NR_pread64, fd, buf, n, (ulong) off, (ulong) (off >> 32));
#else
    return sys4(NR_pread64, fd, buf, n, off);
#endif
}
static inline slong pwrite64_(slong fd, const void *buf, ulong n, unsigned long long off) {
#if defined(__i386__)
    return sys5(NR_pwrite64, fd, buf, n, (ulong) off, (ulong) (off >> 32));
#else
    return sys4(NR_pwrite64, fd, buf, n, off);
#endif
}
static inline slong ftruncate64_(slong fd, unsigned long long len) {
#if defined(__i386__)
    return sys3(NR_ftruncate64, fd, (ulong) len, (ulong) (len >> 32));
#else
    return sys2(NR_ftruncate, fd, len);
#endif
}

static inline slong waitpid_(slong pid, int *status) {
#if defined(__i386__)
    return sys3(NR_waitpid, pid, status, 0);
#else
    return sys4(NR_wait4, pid, status, 0, 0);
#endif
}

// A timespec in the guest's own long, which is what both ABIs' clock and
// futex calls take.
struct timespec_ {
    slong sec, nsec;
};

static inline struct timespec_ now_(int clock) {
    struct timespec_ t = {0, 0};
    sys2(NR_clock_gettime, clock, &t);
    return t;
}

static inline struct timespec_ plus_ms_(struct timespec_ t, slong ms) {
    t.nsec += ms * 1000000;
    t.sec += t.nsec / 1000000000;
    t.nsec %= 1000000000;
    return t;
}

static inline int before_(struct timespec_ a, struct timespec_ b) {
    return a.sec < b.sec || (a.sec == b.sec && a.nsec < b.nsec);
}

static inline int is_err(slong r) {
    return (ulong) r >= (ulong) -4095;
}

static inline void sleep_ms(long ms) {
    struct { slong sec, nsec; } ts = {ms / 1000, (ms % 1000) * 1000000};
    sys2(NR_nanosleep, &ts, 0);
}
