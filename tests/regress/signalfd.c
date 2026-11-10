// A signalfd reads the signals in its mask pending for its reader. SIGCHLD,
// ignored by default, is still queued while blocked, so a parent waiting on
// a signalfd hears its child exit: what util-linux's script does to give a
// program a terminal. The read is one signalfd_siginfo, the child's pid in
// it and how it ended as Linux says it — CLD_EXITED and its exit code, which
// is what script reaps on — and polling it is readable only while one is
// pending.
#include "sys.h"

enum { SIG_BLOCK = 0, SIGCHLD = 17, POLLIN = 1 };

struct pollfd_ {
    int fd;
    short events, revents;
};

struct signalfd_siginfo_ {
    unsigned signo;
    int err, code;
    unsigned pid, uid;
    int fd;
    unsigned tid, band, overrun, trapno;
    int status;
    char rest[128 - 44];
};

void _start(void) {
    unsigned long long mask = 1ull << (SIGCHLD - 1);
    check(sys4(NR_rt_sigprocmask, SIG_BLOCK, &mask, 0, 8) == 0, "blocking SIGCHLD failed");
    slong sfd = sys4(NR_signalfd4, -1, &mask, 8, 0);
    check(sfd >= 0, "signalfd4 failed");

    struct pollfd_ p = {.fd = (int) sfd, .events = POLLIN};
    check(sys3(NR_poll, &p, 1, 0) == 0, "the signalfd polled readable with nothing pending");

    slong pid = sys0(NR_fork);
    check(pid >= 0, "fork failed");
    if (pid == 0)
        sys1(NR_exit_group, 3);

    p.revents = 0;
    check(sys3(NR_poll, &p, 1, 5000) == 1 && (p.revents & POLLIN), "the child's SIGCHLD did not make the signalfd readable");
    struct signalfd_siginfo_ info;
    check(sys3(NR_read, sfd, &info, sizeof(info)) == sizeof(info), "reading the signalfd failed");
    check(info.signo == SIGCHLD, "the signal read was not SIGCHLD");
    check(info.pid == (unsigned) pid, "the signal did not name the child");
    check(info.code == 1 && info.status == 3, "the signal did not say the child exited with 3");
    int status = -1;
    check(waitpid_(pid, &status) == pid, "waitpid failed");
    finish(1, 0);
}
