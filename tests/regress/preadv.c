// preadv and preadv2 read their iovecs from an offset, each from where the
// last ended; preadv2 at offset -1 reads from the file offset as readv does,
// and with RWF_NOWAIT is EAGAIN on a pipe with nothing in it rather than a
// wait. x86-64 alone: i386 has neither.
#include "sys.h"

#if defined(__x86_64__)
enum { RWF_NOWAIT = 8 };

struct iovec_ {
    void *base;
    unsigned long len;
};

void _start(void) {
    const char *path = "/tmp/preadv";
    slong fd = sys3(NR_open, path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open failed");
    check(sys3(NR_write, fd, "abcdefgh", 8) == 8, "write failed");

    char a[3] = {0}, b[3] = {0};
    struct iovec_ iov[2] = {{a, 2}, {b, 2}};
    check(sys4(NR_preadv, fd, iov, 2, 2) == 4, "preadv did not read four bytes");
    check(a[0] == 'c' && a[1] == 'd' && b[0] == 'e' && b[1] == 'f', "preadv read the wrong bytes");
    check(sys4(NR_preadv, fd, iov, 2, -1) == -EINVAL, "preadv at -1 was not EINVAL");

    check(sys3(NR_lseek, fd, 6, SEEK_SET) == 6, "lseek failed");
    check(sys6(NR_preadv2, fd, (slong) iov, 1, -1, 0, 0) == 2, "preadv2 at -1 did not read");
    check(a[0] == 'g' && a[1] == 'h', "preadv2 at -1 did not read from the file offset");
    sys1(NR_close, fd);
    sys1(NR_unlink, path);

    int fds[2];
    check(sys1(NR_pipe, fds) == 0, "pipe failed");
    check(sys6(NR_preadv2, fds[0], (slong) iov, 1, -1, 0, RWF_NOWAIT) == -EAGAIN,
            "RWF_NOWAIT on an empty pipe was not EAGAIN");
    check(sys3(NR_write, fds[1], "z", 1) == 1, "pipe write failed");
    check(sys6(NR_preadv2, fds[0], (slong) iov, 1, -1, 0, RWF_NOWAIT) == 1, "RWF_NOWAIT did not read what was there");
    check(a[0] == 'z', "RWF_NOWAIT read the wrong byte");
    finish(1, 0);
}
#else
void _start(void) {
    finish(1, 0);
}
#endif
