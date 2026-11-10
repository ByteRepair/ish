// The structs whose layout differs between i386 and x86-64, read back field
// by field. Each is defined here from Linux's own headers for the
// architecture, not from iSH's, so a field the kernel writes in the wrong
// place shows up as a wrong value rather than a plausible one.
#include "sys.h"

enum { AT_EMPTY_PATH = 0x1000, CLOCK_REALTIME = 0, RUSAGE_SELF = 0, S_IFMT = 0170000, S_IFREG = 0100000 };

#if defined(__x86_64__)
struct kstat {
    ulong dev, ino, nlink;
    unsigned mode, uid, gid, pad0;
    ulong rdev;
    long size, blksize, blocks;
    ulong atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    long unused[3];
};
_Static_assert(sizeof(struct kstat) == 144, "x86-64 stat");
#define NR_STAT NR_stat
#define NR_FSTAT NR_fstat
#define NR_FSTATAT NR_newfstatat
#else
struct kstat {
    unsigned long long dev;
    unsigned char pad0[4];
    ulong ino32;
    unsigned mode, nlink;
    ulong uid, gid;
    unsigned long long rdev;
    unsigned char pad3[4];
    long long size;
    ulong blksize;
    unsigned long long blocks;
    ulong atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    unsigned long long ino;
};
_Static_assert(sizeof(struct kstat) == 96, "i386 stat64");
#define NR_STAT NR_stat64
#define NR_FSTAT NR_fstat64
#define NR_FSTATAT NR_fstatat64
#endif

struct timespec { long sec, nsec; };
struct timeval { long sec, usec; };
struct iovec { const void *base; ulong len; };
struct rusage { struct timeval utime, stime; long rest[14]; };
// i386's statfs is the same longs, 32 bits each
struct statfs { long type, bsize; ulong blocks, bfree, bavail, files, ffree; int fsid[2]; long namelen, frsize, flags, spare[4]; };

static void check_stat(const struct kstat *st, const char *what) {
    check((st->mode & S_IFMT) == S_IFREG, what);
    check(st->size == 11, what);
    check(st->nlink == 1, what);
    check(st->blksize > 0 && (st->blksize & (st->blksize - 1)) == 0, what);
    check(st->mtime_nsec < 1000000000 && st->mtime > 1700000000, what);
}

void _start(void) {
    const char *path = "/tmp/abi_structs";
    slong fd = sys3(NR_open, path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open failed");

    // writev's iovec: two pieces, all of each
    struct iovec iov[2] = {{"hello ", 6}, {"world", 5}};
    check(sys3(NR_writev, fd, iov, 2) == 11, "writev did not write both iovecs whole");
    char back[12] = {0};
    check(pread64_(fd, back, 11, 0) == 11, "pread after writev failed");
    const char *want = "hello world";
    for (int i = 0; i < 11; i++)
        check(back[i] == want[i], "writev's iovecs landed in the wrong place");

    struct kstat st;
    check(sys2(NR_FSTAT, fd, &st) == 0, "fstat failed");
    check_stat(&st, "fstat's struct stat has a field out of place");
    check(sys2(NR_STAT, path, &st) == 0, "stat failed");
    check_stat(&st, "stat's struct stat has a field out of place");
    check(sys4(NR_FSTATAT, fd, "", &st, AT_EMPTY_PATH) == 0, "fstatat with AT_EMPTY_PATH failed");
    check_stat(&st, "fstatat's struct stat has a field out of place");

    struct timespec ts = {0, -1};
    check(sys2(NR_clock_gettime, CLOCK_REALTIME, &ts) == 0, "clock_gettime failed");
    check(ts.sec > 1700000000 && ts.nsec >= 0 && ts.nsec < 1000000000, "clock_gettime's timespec is out of place");

    struct rusage ru;
    for (int i = 0; i < 14; i++) ru.rest[i] = -1;
    check(sys2(NR_getrusage, RUSAGE_SELF, &ru) == 0, "getrusage failed");
    check(ru.utime.usec < 1000000 && ru.stime.usec < 1000000, "getrusage's timevals are out of place");
    check(ru.rest[13] != -1, "getrusage did not fill the whole struct");

    struct statfs sf;
    sf.namelen = -1;
    check(sys2(NR_statfs, "/", &sf) == 0, "statfs failed");
    check(sf.bsize > 0 && (sf.bsize & (sf.bsize - 1)) == 0, "statfs's bsize is out of place");
    check(sf.namelen > 0 && sf.namelen <= 1024, "statfs's namelen is out of place");

    sys1(NR_close, fd);
    sys1(NR_unlink, path);
    finish(1, 0);
}
