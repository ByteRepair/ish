// signalfd: a file that reads the signals in its mask pending for whoever
// reads it, one signalfd_siginfo each, and polls readable while there is one.
// A program blocks those signals and takes them here instead of in a handler:
// util-linux's script waits on its terminal and its child's SIGCHLD together
// this way, and is what gives a CLI's program a terminal.
//
// A pending signal is the reader's own (current's), as on Linux, so a poll is
// answered for the thread polling. Every signalfd is on one list, woken when a
// signal is queued for anyone: there are few of them, and a wakeup only makes
// a poller look again.
#include <string.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "fs/poll.h"

#define SFD_NONBLOCK_ O_NONBLOCK_
#define SFD_CLOEXEC_ O_CLOEXEC_

struct signalfd_siginfo_ {
    uint32_t signo;
    int32_t err;
    int32_t code;
    uint32_t pid;
    uint32_t uid;
    int32_t fd;
    uint32_t tid;
    uint32_t band;
    uint32_t overrun;
    uint32_t trapno;
    int32_t status;
    int32_t int_;
    uint64_t ptr;
    uint64_t utime;
    uint64_t stime;
    uint64_t addr;
    uint16_t addr_lsb;
    uint8_t pad[46];
};
_Static_assert(sizeof(struct signalfd_siginfo_) == 128, "signalfd_siginfo is 128 bytes");

static struct fd_ops signalfd_ops;
static lock_t signalfds_lock = LOCK_INITIALIZER;
static struct list signalfds = LIST_INITIALIZER(signalfds);

void signalfd_notify(void) {
    lock(&signalfds_lock);
    struct fd *fd;
    list_for_each_entry(&signalfds, fd, signalfd.fds) {
        lock(&fd->lock);
        notify(&fd->cond);
        unlock(&fd->lock);
        poll_wakeup(fd, POLL_READ);
    }
    unlock(&signalfds_lock);
}

static slong_t do_signalfd(fd_t f, addr_t mask_addr, uint_t size, int_t flags) {
    if (flags & ~(SFD_NONBLOCK_ | SFD_CLOEXEC_))
        return _EINVAL;
    if (size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    // SIGKILL and SIGSTOP are never taken this way
    sigset_del(&mask, SIGKILL_);
    sigset_del(&mask, SIGSTOP_);

    if (f != -1) {
        struct fd *fd = f_get(f);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops != &signalfd_ops)
            return _EINVAL;
        lock(&fd->lock);
        fd->signalfd.mask = mask;
        unlock(&fd->lock);
        signalfd_notify();
        return f;
    }

    struct fd *fd = adhoc_fd_create(&signalfd_ops);
    if (fd == NULL)
        return _ENOMEM;
    fd->signalfd.mask = mask;
    lock(&signalfds_lock);
    list_add(&signalfds, &fd->signalfd.fds);
    unlock(&signalfds_lock);
    return f_install(fd, flags);
}

slong_t sys_signalfd4(fd_t f, addr_t mask_addr, uint_t size, int_t flags) {
    STRACE("signalfd4(%d, %#x, %d, %#x)", f, mask_addr, size, flags);
    return do_signalfd(f, mask_addr, size, flags);
}

slong_t sys_signalfd(fd_t f, addr_t mask_addr, uint_t size) {
    STRACE("signalfd(%d, %#x, %d)", f, mask_addr, size);
    return do_signalfd(f, mask_addr, size, 0);
}

static void fill_siginfo(struct signalfd_siginfo_ *out, const struct siginfo_ *info) {
    memset(out, 0, sizeof(*out));
    out->signo = info->sig;
    out->err = info->sig_errno;
    out->code = info->code;
    switch (info->sig) {
        case SIGCHLD_:
            out->pid = info->child.pid;
            out->uid = info->child.uid;
            out->status = info->child.status;
            out->utime = info->child.utime;
            out->stime = info->child.stime;
            break;
        case SIGSEGV_: case SIGBUS_: case SIGILL_: case SIGFPE_:
            out->addr = info->fault.addr;
            break;
        default:
            out->pid = info->kill.pid;
            out->uid = info->kill.uid;
            break;
    }
}

static ssize_t signalfd_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize < sizeof(struct signalfd_siginfo_))
        return _EINVAL;
    lock(&fd->lock);
    sigset_t_ mask = fd->signalfd.mask;
    size_t done = 0;
    while (true) {
        lock(&current->sighand->lock);
        struct siginfo_ info;
        while (done + sizeof(struct signalfd_siginfo_) <= bufsize && dequeue_signal(mask, &info)) {
            fill_siginfo((struct signalfd_siginfo_ *) ((char *) buf + done), &info);
            done += sizeof(struct signalfd_siginfo_);
        }
        unlock(&current->sighand->lock);
        if (done > 0)
            break;
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        if (wait_for(&fd->cond, &fd->lock, NULL)) {
            unlock(&fd->lock);
            return _EINTR;
        }
        mask = fd->signalfd.mask;
    }
    unlock(&fd->lock);
    return done;
}

static int signalfd_poll(struct fd *fd) {
    lock(&fd->lock);
    sigset_t_ mask = fd->signalfd.mask;
    unlock(&fd->lock);
    lock(&current->sighand->lock);
    bool ready = (current->pending & mask) != 0;
    unlock(&current->sighand->lock);
    return ready ? POLL_READ : 0;
}

static int signalfd_close(struct fd *fd) {
    lock(&signalfds_lock);
    list_remove(&fd->signalfd.fds);
    unlock(&signalfds_lock);
    return 0;
}

static struct fd_ops signalfd_ops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
    .close = signalfd_close,
};
