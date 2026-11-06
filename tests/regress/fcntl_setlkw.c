// F_SETLKW and F_SETLKW64 wait for a conflicting lock to go, where F_SETLK
// and F_SETLK64 answer EAGAIN at once. The parent holds a write lock across
// a fork; the child's waiting request must return 0 only after the parent
// lets go, and its non-waiting request must fail while the parent holds it.
#include "sys.h"

struct flock32 { short type, whence; long start, len; int pid; };
struct flock64 { short type, whence; long long start, len; int pid; } __attribute__((packed));

static slong lock32(slong fd, int cmd, short type) {
    struct flock32 l = {type, SEEK_SET, 0, 0, 0};
    return sys3(NR_fcntl, fd, cmd, &l);
}
static slong lock64(slong fd, int cmd, short type) {
    struct flock64 l = {type, SEEK_SET, 0, 0, 0};
    return sys3(NR_fcntl64, fd, cmd, &l);
}

static void child(const char *path, int wide) {
    slong fd = sys3(NR_open, path, O_RDWR, 0);
    check(fd >= 0, "child open failed");
    slong (*lock)(slong, int, short) = wide ? lock64 : lock32;
    int setlk = wide ? F_SETLK64 : F_SETLK, setlkw = wide ? F_SETLKW64 : F_SETLKW;
    check(lock(fd, setlk, F_WRLCK) == -EAGAIN, "F_SETLK did not refuse a held lock");
    slong r = lock(fd, setlkw, F_WRLCK);
    check(r != -EAGAIN, "F_SETLKW did not wait for the lock");
    check(r == 0, "F_SETLKW failed");
    sys1(NR_exit_group, 0);
}

static void run(const char *path, int wide) {
    slong fd = sys3(NR_open, path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open failed");
    check((wide ? lock64 : lock32)(fd, wide ? F_SETLK64 : F_SETLK, F_WRLCK) == 0, "parent lock failed");
    slong pid = sys0(NR_fork);
    check(pid >= 0, "fork failed");
    if (pid == 0)
        child(path, wide);
    sleep_ms(200);
    check((wide ? lock64 : lock32)(fd, wide ? F_SETLK64 : F_SETLK, F_UNLCK) == 0, "parent unlock failed");
    int status = 0;
    check(sys3(NR_waitpid, pid, &status, 0) == pid, "waitpid failed");
    check(status == 0, wide ? "child's 64-bit locks misbehaved" : "child's 32-bit locks misbehaved");
    sys1(NR_close, fd);
}

void _start(void) {
    const char *path = "/tmp/fcntl_setlkw";
    run(path, 0);
    run(path, 1);
    sys1(NR_unlink, path);
    finish(1, 0);
}
