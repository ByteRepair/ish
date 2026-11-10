// lseek's offset is a signed off_t: -1 from the end of a four-byte file is
// offset 3, and -2 from there is 1, not offsets just under 4 GiB.
#include "sys.h"

void _start(void) {
    const char *path = "/tmp/lseek_negative";
    slong fd = sys3(NR_open, path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open failed");
    check(sys3(NR_write, fd, "abcd", 4) == 4, "write failed");
    check(sys3(NR_lseek, fd, -1, SEEK_END) == 3, "lseek -1 from the end did not land at 3");
    check(sys3(NR_lseek, fd, -2, SEEK_CUR) == 1, "lseek -2 from the current offset did not land at 1");
    char c = 0;
    check(sys3(NR_read, fd, &c, 1) == 1 && c == 'b', "read after the seeks did not see 'b'");
    sys1(NR_close, fd);
    sys1(NR_unlink, path);
    finish(1, 0);
}
