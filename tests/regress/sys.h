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
    // around the trap rather than named as a constraint.
    __asm__ volatile("push %%ebp\n\tmov %7, %%ebp\n\tint $0x80\n\tpop %%ebp"
                     : "=a"(ret)
                     : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "m"(f)
                     : "memory");
    return ret;
}
enum {
    NR_exit = 1, NR_fork = 2, NR_read = 3, NR_write = 4, NR_open = 5,
    NR_close = 6, NR_waitpid = 7, NR_unlink = 10, NR_fcntl = 55,
    NR_old_mmap = 90, NR_munmap = 91, NR_nanosleep = 162, NR_mremap = 163,
    NR_pread64 = 180, NR_pwrite64 = 181, NR_ftruncate64 = 194,
    NR_fcntl64 = 221, NR_exit_group = 252,
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
    F_WRLCK = 1, F_UNLCK = 2, SEEK_SET = 0,
    MREMAP_MAYMOVE = 1,
    EAGAIN = 11, EFAULT = 14,
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
#endif
}

static inline int is_err(slong r) {
    return (ulong) r >= (ulong) -4095;
}

static inline void sleep_ms(long ms) {
    struct { slong sec, nsec; } ts = {ms / 1000, (ms % 1000) * 1000000};
    sys2(NR_nanosleep, &ts, 0);
}
