// Two numbers for one open file (a dup) are two epoll entries, as on Linux:
// adding the second is not EEXIST, and each reports the file ready. Bun
// registers a dup of its stdin beside it, and a terminal is one open file on
// fds 0, 1 and 2. A number already added is still EEXIST.
#include "sys.h"

enum { EPOLL_CTL_ADD = 1, EPOLLIN = 1 };

struct epoll_event_ {
    unsigned events;
    unsigned long long data;
} __attribute__((packed));

void _start(void) {
    int fds[2];
    check(sys1(NR_pipe, fds) == 0, "pipe failed");
    slong copy = sys1(NR_dup, fds[0]);
    check(copy >= 0, "dup failed");
    slong ep = sys1(NR_epoll_create1, 0);
    check(ep >= 0, "epoll_create1 failed");

    struct epoll_event_ ev = {.events = EPOLLIN, .data = 10};
    check(sys4(NR_epoll_ctl, ep, EPOLL_CTL_ADD, fds[0], &ev) == 0, "adding the pipe failed");
    ev.data = 20;
    check(sys4(NR_epoll_ctl, ep, EPOLL_CTL_ADD, copy, &ev) == 0, "adding its dup was refused");
    check(sys4(NR_epoll_ctl, ep, EPOLL_CTL_ADD, copy, &ev) == -EEXIST, "adding the dup twice was not EEXIST");

    check(sys3(NR_write, fds[1], "x", 1) == 1, "write failed");
    struct epoll_event_ out[4] = {{0}};
    slong n = sys4(NR_epoll_wait, ep, out, 4, 1000);
    check(n == 2, "epoll_wait did not report both entries");
    unsigned long long seen = out[0].data + out[1].data;
    check(seen == 30, "the entries reported were not the pipe and its dup");
    finish(1, 0);
}
