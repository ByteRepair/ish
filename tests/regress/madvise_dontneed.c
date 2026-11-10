// MADV_DONTNEED on a private anonymous mapping: the pages read as zero
// after, which allocators that decommit with it (jemalloc, JavaScriptCore's
// libpas) count on, handing the memory out again as zeroed. A private page
// of a file reads as the file does again. A page shared copy-on-write with a
// parent is dropped for the child alone. A shared mapping keeps what it
// holds. A range with a hole in it is ENOMEM, and the part that is mapped is
// still discarded.
#include "sys.h"

enum { MADV_DONTNEED = 4, MAP_SHARED = 1, PAGE = 4096 };

static int all(const unsigned char *p, ulong n, unsigned char v) {
    for (ulong i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

static void fill(unsigned char *p, ulong n, unsigned char v) {
    for (ulong i = 0; i < n; i++)
        p[i] = v;
}

void _start(void) {
    unsigned char *p = mmap_(0, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(!is_err((slong) p), "mmap failed");
    fill(p, 3 * PAGE, 0xab);
    check(sys3(NR_madvise, p + PAGE, PAGE, MADV_DONTNEED) == 0, "madvise failed");
    check(all(p + PAGE, PAGE, 0), "the discarded page did not read as zero");
    check(all(p, PAGE, 0xab) && all(p + 2 * PAGE, PAGE, 0xab), "a page beside it changed");
    fill(p + PAGE, PAGE, 0xcd);
    check(all(p + PAGE, PAGE, 0xcd), "the discarded page could not be written again");

    check(sys3(NR_madvise, p + 1, PAGE, MADV_DONTNEED) == -EINVAL, "an unaligned address was not EINVAL");
    check(sys2(NR_munmap, p + 2 * PAGE, PAGE) == 0, "munmap failed");
    check(sys3(NR_madvise, p + PAGE, 2 * PAGE, MADV_DONTNEED) == -ENOMEM, "a range with a hole was not ENOMEM");
    check(all(p + PAGE, PAGE, 0), "the mapped part of a range with a hole was kept");

    // a private page of a file goes back to the file's bytes
    const char *path = "/tmp/madvise_dontneed";
    slong fd = sys3(NR_open, path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open failed");
    static unsigned char page[2 * PAGE];
    fill(page, sizeof(page), 0x11);
    check(pwrite64_(fd, page, sizeof(page), 0) == sizeof(page), "pwrite64 failed");
    unsigned char *f = mmap_(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check(!is_err((slong) f), "file mmap failed");
    fill(f, 2 * PAGE, 0x22);
    check(sys3(NR_madvise, f + PAGE, PAGE, MADV_DONTNEED) == 0, "madvise of a file's page failed");
    check(all(f + PAGE, PAGE, 0x11), "a file's private page did not read as the file again");
    check(all(f, PAGE, 0x22), "the file's page beside it changed");
    fill(f + PAGE, PAGE, 0x33);
    check(pread64_(fd, page, PAGE, PAGE) == PAGE && all(page, PAGE, 0x11), "a private write reached the file");
    sys1(NR_close, fd);
    sys1(NR_unlink, path);

    // the child drops its copy; the parent's stays
    slong pid = sys0(NR_fork);
    check(pid >= 0, "fork failed");
    if (pid == 0) {
        sys3(NR_madvise, p, PAGE, MADV_DONTNEED);
        sys1(NR_exit_group, all(p, PAGE, 0) ? 0 : 1);
    }
    int status = -1;
    check(waitpid_(pid, &status) == pid, "waitpid failed");
    check(status == 0, "the child's page did not read as zero");
    check(all(p, PAGE, 0xab), "the child's madvise reached the parent's page");

    unsigned char *s = mmap_(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check(!is_err((slong) s), "shared mmap failed");
    fill(s, PAGE, 0x5a);
    check(sys3(NR_madvise, s, PAGE, MADV_DONTNEED) == 0, "madvise of a shared page failed");
    check(all(s, PAGE, 0x5a), "a shared page lost what it held");
    finish(1, 0);
}
