// pread64 and pwrite64 take their offset as two 32-bit halves on i386; one
// past 4 GiB must reach the file there, not at its low half.
#include "sys.h"

#define HIGH 0x100000001ull // 4 GiB + 1

void _start(void) {
    const char *path = "/tmp/pread_high";
    slong fd = sys3(NR_open, path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open failed");
    check(ftruncate64_(fd, 0x100001000ull) == 0, "ftruncate64 past 4 GiB failed");
    check(pwrite64_(fd, "x", 1, HIGH) == 1, "pwrite64 failed");
    char c = 0;
    check(pread64_(fd, &c, 1, 1) == 1 && c == 0, "pwrite64 landed at the offset's low half");
    check(pread64_(fd, &c, 1, HIGH) == 1, "pread64 failed");
    check(c == 'x', "pread64 past 4 GiB did not read what pwrite64 wrote");
    sys1(NR_close, fd);
    sys1(NR_unlink, path);
    finish(1, 0);
}
