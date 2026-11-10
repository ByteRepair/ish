#include "debug.h"
#include <string.h>
#include <signal.h>
#include "kernel/calls.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/vdso.h"
#include "emu/interrupt.h"

#if is_gcc(9)
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif

int xsave_extra = 0;
int fxsave_extra = 0;
static void sigmask_set(sigset_t_ set);
static void altstack_to_user(struct sighand *sighand, struct stack_t_ *user_stack);
static bool is_on_altstack(addr_t sp, struct sighand *sighand);

static int signal_is_blockable(int sig) {
    return sig != SIGKILL_ && sig != SIGSTOP_;
}

#define UNBLOCKABLE_MASK (sig_mask(SIGKILL_) | sig_mask(SIGSTOP_))

#define SIGNAL_IGNORE 0
#define SIGNAL_KILL 1
#define SIGNAL_CALL_HANDLER 2
#define SIGNAL_STOP 3

static int signal_action(struct sighand *sighand, int sig) {
    if (signal_is_blockable(sig)) {
        struct sigaction_ *action = &sighand->action[sig];
        if (action->handler == SIG_IGN_)
            return SIGNAL_IGNORE;
        if (action->handler != SIG_DFL_)
            return SIGNAL_CALL_HANDLER;
    }

    switch (sig) {
        case SIGURG_: case SIGCONT_: case SIGCHLD_:
        case SIGIO_: case SIGWINCH_:
            return SIGNAL_IGNORE;

        case SIGSTOP_: case SIGTSTP_: case SIGTTIN_: case SIGTTOU_:
            return SIGNAL_STOP;

        default:
            return SIGNAL_KILL;
    }
}

static void deliver_signal_unlocked(struct task *task, int sig, struct siginfo_ info) {
    if (sigset_has(task->pending, sig))
        return;

    sigset_add(&task->pending, sig);
    struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
    sigqueue->info = info;
    sigqueue->info.sig = sig;
    list_add_tail(&task->queue, &sigqueue->queue);

    if (sigset_has(task->blocked & ~task->waiting, sig) && signal_is_blockable(sig))
        return;

    if (task != current) {
        pthread_kill(task->thread, SIGUSR1);

        // wake up any pthread condition waiters
        // actual madness, I hope to god it's correct
        // must release the sighand lock while going insane, to avoid a deadlock
        unlock(&task->sighand->lock);
retry:
        lock(&task->waiting_cond_lock);
        if (task->waiting_cond != NULL) {
            bool mine = false;
            if (trylock(task->waiting_lock) == EBUSY) {
                if (pthread_equal(task->waiting_lock->owner, pthread_self()))
                    mine = true;
                if (!mine) {
                    unlock(&task->waiting_cond_lock);
                    goto retry;
                }
            }
            notify(task->waiting_cond);
            if (!mine)
                unlock(task->waiting_lock);
        }
        unlock(&task->waiting_cond_lock);
        lock(&task->sighand->lock);
    }
}

void deliver_signal(struct task *task, int sig, struct siginfo_ info) {
    lock(&task->sighand->lock);
    deliver_signal_unlocked(task, sig, info);
    unlock(&task->sighand->lock);
    signalfd_notify();
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    // signal zero is for testing whether a process exists
    if (sig == 0)
        return;
    if (task->zombie || task->exiting)
        return;

    // A blocked signal is queued whatever its action, as on Linux: the
    // handler may change before it is unblocked, and one taken by
    // sigtimedwait or a signalfd never reaches a handler at all. SIGCHLD,
    // ignored by default, is what script waits on that way.
    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    bool queued = signal_action(sighand, sig) != SIGNAL_IGNORE || sigset_has(task->blocked, sig);
    if (queued)
        deliver_signal_unlocked(task, sig, info);
    unlock(&sighand->lock);
    // A signal ignored and not queued is nothing a signalfd could read.
    if (queued)
        signalfd_notify();

    if (sig == SIGCONT_ || sig == SIGKILL_) {
        lock(&task->group->lock);
        task->group->stopped = false;
        notify(&task->group->stopped_cond);
        unlock(&task->group->lock);
    }
}

bool try_self_signal(int sig) {
    assert(sig == SIGTTIN_ || sig == SIGTTOU_);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    bool can_send = signal_action(sighand, sig) != SIGNAL_IGNORE &&
        !sigset_has(current->blocked, sig);
    if (can_send)
        deliver_signal_unlocked(current, sig, SIGINFO_NIL);
    unlock(&sighand->lock);
    return can_send;
}

int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    lock(&pids_lock);
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        send_signal(tgroup->leader, sig, info);
    }
    unlock(&pids_lock);
    return 0;
}

#if !GUEST_X86_64
static addr_t sigreturn_trampoline(const char *name) {
    addr_t sigreturn_addr = vdso_symbol(name);
    if (sigreturn_addr == 0) {
        die("sigreturn not found in vdso, this should never happen");
    }
    return current->mm->vdso + sigreturn_addr;
}

static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu) {
    sc->ax = cpu->eax;
    sc->bx = cpu->ebx;
    sc->cx = cpu->ecx;
    sc->dx = cpu->edx;
    sc->di = cpu->edi;
    sc->si = cpu->esi;
    sc->bp = cpu->ebp;
    sc->sp = sc->sp_at_signal = cpu->esp;
    sc->ip = cpu->eip;
    collapse_flags(cpu);
    sc->flags = cpu->eflags;
    sc->trapno = cpu->trapno;
    if (cpu->trapno == INT_GPF)
        sc->cr2 = cpu->segfault_addr;
    // TODO more shit
    sc->oldmask = current->blocked & 0xffffffff;
}

static void setup_sigframe(struct siginfo_ *info, struct sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_sigreturn");
    frame->sig = info->sig;
    setup_sigcontext(&frame->sc, &current->cpu);
    frame->extramask = current->blocked >> 32;

    static const struct {
        uint16_t popmov;
        uint32_t nr_sigreturn;
        uint16_t int80;
    } __attribute__((packed)) retcode = {
        .popmov = 0xb858,
        .nr_sigreturn = 113,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &retcode, sizeof(retcode));
}

static void setup_rt_sigframe(struct siginfo_ *info, struct rt_sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_rt_sigreturn");
    frame->sig = info->sig;
    frame->info = *info;
    frame->uc.flags = 0;
    frame->uc.link = 0;
    altstack_to_user(current->sighand, &frame->uc.stack);
    setup_sigcontext(&frame->uc.mcontext, &current->cpu);
    frame->uc.sigmask = current->blocked;

    static const struct {
        uint8_t mov;
        uint32_t nr_rt_sigreturn;
        uint16_t int80;
        uint8_t pad;
    } __attribute__((packed)) rt_retcode = {
        .mov = 0xb8,
        .nr_rt_sigreturn = 173,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &rt_retcode, sizeof(rt_retcode));
}

#endif

#if GUEST_X86_64

// blink keeps x87 registers as doubles; the fxsave area stores them 80-bit
// extended. These convert between the two, exactly for everything a double
// holds (a double is a subset of extended); a subnormal double is flushed to
// zero, which loses only values a guest rarely produces and blink's own x87
// would not round-trip anyway.
static void dbl_to_f80(double d, qword_t *sig, word_t *sexp) {
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    uint64_t sign = b >> 63;
    uint32_t e = (b >> 52) & 0x7ff;
    uint64_t f = b & 0xfffffffffffffULL;
    uint64_t s;
    uint16_t x;
    if (e == 0x7ff) {
        s = f ? 0xc000000000000000ULL | (f >> 1) : 0x8000000000000000ULL;
        x = 0x7fff;
    } else if (e == 0 && f == 0) {
        s = 0;
        x = 0;
    } else if (e == 0) {
        s = 0; // subnormal double, flushed to zero (see above)
        x = 0;
    } else {
        s = (f | 0x10000000000000ULL) << 11;
        x = (uint16_t) (e - 1023 + 16383);
    }
    *sig = s;
    *sexp = (word_t) ((sign << 15) | x);
}

static double f80_to_dbl(qword_t s, word_t sexp) {
    uint64_t sign = (uint64_t) (sexp >> 15);
    uint32_t x = sexp & 0x7fff;
    uint64_t b;
    if (x == 0x7fff) {
        uint64_t f = (s & 0x7fffffffffffffffULL) >> 11;
        b = (sign << 63) | (0x7ffULL << 52) | f;
    } else if (x == 0 || s == 0) {
        b = sign << 63;
    } else {
        int64_t e = (int64_t) x - 16383 + 1023;
        if (e >= 0x7ff) {
            b = (sign << 63) | (0x7ffULL << 52);
        } else if (e <= 0) {
            b = sign << 63;
        } else {
            uint64_t f = (s >> 11) & 0xfffffffffffffULL;
            b = (sign << 63) | ((uint64_t) e << 52) | f;
        }
    }
    double d;
    memcpy(&d, &b, sizeof(d));
    return d;
}

// Fill the frame from the current registers; the fxsave area is separate and
// 16-byte aligned below the frame, its guest address written into the
// sigcontext once the frame is placed. pretcode is the handler's SA_RESTORER.
static void setup_rt_sigframe_64(struct siginfo_ *info, struct rt_sigframe_64 *frame,
                                  struct fpstate_64 *fp) {
    struct sigaction_ *action = &current->sighand->action[info->sig];
    struct x86_64_regs *r = &current->cpu.r;

    frame->pretcode = action->restorer;
    frame->uc.flags = 0;
    frame->uc.link = 0;
    altstack_to_user(current->sighand, &frame->uc.stack);

    struct sigcontext_64 *sc = &frame->uc.mcontext;
    sc->r8 = r->gpr[8]; sc->r9 = r->gpr[9]; sc->r10 = r->gpr[10]; sc->r11 = r->gpr[11];
    sc->r12 = r->gpr[12]; sc->r13 = r->gpr[13]; sc->r14 = r->gpr[14]; sc->r15 = r->gpr[15];
    sc->rdi = r->gpr[7]; sc->rsi = r->gpr[6]; sc->rbp = r->gpr[5]; sc->rbx = r->gpr[3];
    sc->rdx = r->gpr[2]; sc->rax = r->gpr[0]; sc->rcx = r->gpr[1]; sc->rsp = r->gpr[4];
    sc->rip = r->rip;
    sc->eflags = r->rflags;
    sc->cs = sc->gs = sc->fs = sc->ss = 0;
    sc->err = 0;
    sc->trapno = current->cpu.trapno;
    sc->oldmask = current->blocked;
    sc->cr2 = current->cpu.trapno == INT_GPF ? current->cpu.segfault_addr : 0;
    sc->fpstate = 0; // the guest address, written once the frame is placed
    memset(sc->reserved1, 0, sizeof(sc->reserved1));
    frame->uc.sigmask = current->blocked;
    frame->info = *info;

    memset(fp, 0, sizeof(*fp));
    fp->cw = r->x87.cw;
    fp->sw = r->x87.sw;
    fp->tw = r->x87.tw;
    fp->mxcsr = r->mxcsr;
    fp->mxcsr_mask = 0xffff;
    for (int i = 0; i < 8; i++)
        dbl_to_f80(r->x87.st[i], &fp->st[i].significand, &fp->st[i].signexp);
    memcpy(fp->xmm, r->xmm, sizeof(r->xmm));
}

#endif

static void receive_signal(struct sighand *sighand, struct siginfo_ *info) {
    int sig = info->sig;
    STRACE("%d receiving signal %d\n", current->pid, sig);

    switch (signal_action(sighand, sig)) {
        case SIGNAL_IGNORE:
            return;

        case SIGNAL_STOP:
            lock(&current->group->lock);
            current->group->stopped = true;
            current->group->group_exit_code = sig << 8 | 0x7f;
            unlock(&current->group->lock);
            return;

        case SIGNAL_KILL:
            unlock(&sighand->lock); // do_exit must be called without this lock
            do_exit_group(sig);
    }

    struct sigaction_ *action = &sighand->action[info->sig];
#if GUEST_X86_64
    struct x86_64_regs *r = &current->cpu.r;
    addr_t sp = r->rsp;
    if (sighand->altstack && !is_on_altstack(sp, sighand))
        sp = sighand->altstack + sighand->altstack_size;

    // The frame sits below the current stack, its pretcode at the top so the
    // handler's `ret` pops the restorer; the fxsave area is 16-byte aligned
    // below the frame. rsp is the frame start, exactly as Linux enters a
    // handler, so [rsp] is the return address (pretcode).
    struct rt_sigframe_64 frame;
    struct fpstate_64 fp;
    setup_rt_sigframe_64(info, &frame, &fp);

    sp -= sizeof(frame);
    sp &= ~0xfULL; // the frame is 16-byte aligned
    addr_t frame_addr = sp;
    sp -= sizeof(fp);
    sp &= ~0xfULL;
    addr_t fp_guest = sp;
    frame.uc.mcontext.fpstate = fp_guest;
    r->rsp = frame_addr;

    if (user_write(frame_addr, &frame, sizeof(frame)) || user_write(fp_guest, &fp, sizeof(fp))) {
        printk("failed to install frame for %d at %#llx\n", info->sig, (unsigned long long) frame_addr);
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return;
    }

    // rdi/rsi/rdx = sig, info, ucontext; rip = the handler.
    r->gpr[7] = info->sig;                                      // rdi
    r->gpr[6] = frame_addr + offsetof(struct rt_sigframe_64, info); // rsi
    r->gpr[2] = frame_addr + offsetof(struct rt_sigframe_64, uc);   // rdx
    r->rip = action->handler;

    // The mask: the signal is blocked during the handler unless SA_NODEFER.
    if (!(action->flags & SA_NODEFER_))
        sigset_add(&current->blocked, info->sig);
    current->blocked |= action->mask;

    if (action->flags & SA_RESETHAND_)
        *action = (struct sigaction_) {.handler = SIG_DFL_};
#else
    bool need_siginfo = action->flags & SA_SIGINFO_;

    // setup the frame
    union {
        struct sigframe_ sigframe;
        struct rt_sigframe_ rt_sigframe;
    } frame = {};
    size_t frame_size;
    if (need_siginfo) {
        setup_rt_sigframe(info, &frame.rt_sigframe);
        frame_size = sizeof(frame.rt_sigframe);
    } else {
        setup_sigframe(info, &frame.sigframe);
        frame_size = sizeof(frame.sigframe);
    }

    // set up registers for signal handler
    current->cpu.eax = info->sig;
    current->cpu.eip = sighand->action[info->sig].handler;

    dword_t sp = current->cpu.esp;
    if (sighand->altstack && !is_on_altstack(sp, sighand)) {
        sp = sighand->altstack + sighand->altstack_size;
    }
    if (xsave_extra) {
        // do as the kernel does
        // this is superhypermega condensed version of fpu__alloc_mathframe in
        // arch/x86/kernel/fpu/signal.c
        sp -= xsave_extra;
        sp &=~ 0x3f;
        sp -= fxsave_extra;
    }
    sp -= frame_size;
    // align sp + 4 on a 16-byte boundary because that's what the abi says
    sp = ((sp + 4) & ~0xf) - 4;
    current->cpu.esp = sp;

    // Update the mask. By default the signal will be blocked while in the
    // handler, but sigaction is allowed to customize this.
    if (!(action->flags & SA_NODEFER_))
        sigset_add(&current->blocked, info->sig);
    current->blocked |= action->mask;

    // these have to be filled in after the location of the frame is known
    if (need_siginfo) {
        frame.rt_sigframe.pinfo = sp + offsetof(struct rt_sigframe_, info);
        frame.rt_sigframe.puc = sp + offsetof(struct rt_sigframe_, uc);
        current->cpu.edx = frame.rt_sigframe.pinfo;
        current->cpu.ecx = frame.rt_sigframe.puc;
    }

    // install frame
    if (user_write(sp, &frame, frame_size)) {
        printk("failed to install frame for %d at %#x\n", info->sig, sp);
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
    }

    if (action->flags & SA_RESETHAND_)
        *action = (struct sigaction_) {.handler = SIG_DFL_};
#endif
}

void signal_delivery_stop(int sig, struct siginfo_ *info) {
    lock(&current->ptrace.lock);
    current->ptrace.stopped = true;
    current->ptrace.signal = sig;
    current->ptrace.info = *info;
    unlock(&current->ptrace.lock);
    notify(&current->parent->group->child_exit);
    // TODO add siginfo
    send_signal(current->parent, current->group->leader->exit_signal, SIGINFO_NIL);

    unlock(&current->sighand->lock);
    lock(&current->ptrace.lock);
    while (current->ptrace.stopped) {
        wait_for_ignore_signals(&current->ptrace.cond, &current->ptrace.lock, NULL);
        lock(&current->sighand->lock);
        bool got_sigkill = sigset_has(current->pending, SIGKILL_);
        unlock(&current->sighand->lock);
        if (got_sigkill) {
            STRACE("%d received a SIGKILL in signal delivery stop\n", current->pid);
            unlock(&current->ptrace.lock);
            do_exit_group(SIGKILL_);
        }
    }
    unlock(&current->ptrace.lock);
    lock(&current->sighand->lock);
}

void receive_signals(void) {
    lock(&current->group->lock);
    bool was_stopped = current->group->stopped;
    unlock(&current->group->lock);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);

    // A saved mask means that the last system call was a call like sigsuspend
    // that changes the mask during the call. Only ignore a signal right now if
    // it was both blocked during the call and should still be blocked after
    // the call.
    sigset_t_ blocked = current->blocked;
    if (current->has_saved_mask) {
        blocked &= current->saved_mask;
        current->has_saved_mask = false;
        current->blocked = current->saved_mask;
    }

    struct sigqueue *sigqueue, *tmp;
    list_for_each_entry_safe(&current->queue, sigqueue, tmp, queue) {
        int sig = sigqueue->info.sig;
        if (sigset_has(blocked, sig))
            continue;
        list_remove(&sigqueue->queue);
        sigset_del(&current->pending, sig);

        if (current->ptrace.traced && sig != SIGKILL_) {
            // This notifies the parent, goes to sleep, and waits for the
            // parent to tell it to continue.
            // Any signals received while waiting are left on the queue, except
            // for SIGKILL_, which causes an immediate exit.
            signal_delivery_stop(sig, &sigqueue->info);
        } else {
            receive_signal(sighand, &sigqueue->info);
        }
        free(sigqueue);
    }

    unlock(&sighand->lock);

    // this got moved out of the switch case in receive_signal to fix locking problems
    if (!was_stopped) {
        lock(&current->group->lock);
        bool now_stopped = current->group->stopped;
        unlock(&current->group->lock);
        if (now_stopped) {
            lock(&pids_lock);
            notify(&current->parent->group->child_exit);
            // TODO add siginfo
            send_signal(current->parent, current->group->leader->exit_signal, SIGINFO_NIL);
            unlock(&pids_lock);
        }
    }
}

#if GUEST_X86_64

static void restore_sigcontext_64(struct sigcontext_64 *sc, struct cpu_state *cpu) {
    struct x86_64_regs *r = &cpu->r;
    r->gpr[8] = sc->r8; r->gpr[9] = sc->r9; r->gpr[10] = sc->r10; r->gpr[11] = sc->r11;
    r->gpr[12] = sc->r12; r->gpr[13] = sc->r13; r->gpr[14] = sc->r14; r->gpr[15] = sc->r15;
    r->gpr[7] = sc->rdi; r->gpr[6] = sc->rsi; r->gpr[5] = sc->rbp; r->gpr[3] = sc->rbx;
    r->gpr[2] = sc->rdx; r->gpr[0] = sc->rax; r->gpr[1] = sc->rcx; r->gpr[4] = sc->rsp;
    r->rip = sc->rip;
    r->rflags = sc->eflags;
}

slong_t sys_rt_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct x86_64_regs *r = &cpu->r;

    // rsp points past pretcode (the handler's `ret` popped it), so the frame
    // starts offsetof(uc) below it, exactly as i386's reads its frame.
    addr_t frame_addr = r->rsp - offsetof(struct rt_sigframe_64, uc);
    struct rt_sigframe_64 frame;
    if (user_get(frame_addr, frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }

    restore_sigcontext_64(&frame.uc.mcontext, cpu);

    // The fxsave area was saved below the frame; restore the fpu and xmm
    // registers from it when a handler's guest code touched them.
    if (frame.uc.mcontext.fpstate != 0) {
        struct fpstate_64 fp;
        if (user_get(frame.uc.mcontext.fpstate, fp)) {
            deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
            return _EFAULT;
        }
        r->x87.cw = fp.cw;
        r->x87.sw = fp.sw;
        r->x87.tw = fp.tw;
        r->mxcsr = fp.mxcsr;
        for (int i = 0; i < 8; i++)
            r->x87.st[i] = f80_to_dbl(fp.st[i].significand, fp.st[i].signexp);
        memcpy(r->xmm, fp.xmm, sizeof(r->xmm));
    }

    lock(&current->sighand->lock);
    if (!is_on_altstack(r->rsp, current->sighand) &&
            frame.uc.stack.size >= MINSIGSTKSZ_) {
        current->sighand->altstack = frame.uc.stack.stack;
        current->sighand->altstack_size = frame.uc.stack.size;
    }
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return r->gpr[0];
}

slong_t sys_sigreturn(void) {
    return _ENOSYS;
}

#else

static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    cpu->eax = context->ax;
    cpu->ebx = context->bx;
    cpu->ecx = context->cx;
    cpu->edx = context->dx;
    cpu->edi = context->di;
    cpu->esi = context->si;
    cpu->ebp = context->bp;
    cpu->esp = context->sp;
    cpu->eip = context->ip;
    collapse_flags(cpu);

    // Use AC, RF, OF, DF, TF, SF, ZF, AF, PF, CF
#define USE_FLAGS 0b1010000110111010101
    cpu->eflags = (context->flags & USE_FLAGS) | (cpu->eflags & ~USE_FLAGS);
}

slong_t sys_rt_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_ frame;
    // esp points past the first field of the frame
    if (user_get(cpu->esp - offsetof(struct rt_sigframe_, sig), frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.uc.mcontext, cpu);

    lock(&current->sighand->lock);
    // FIXME this duplicates logic from sys_sigaltstack
    if (!is_on_altstack(cpu->esp, current->sighand) &&
            frame.uc.stack.size >= MINSIGSTKSZ_) {
        current->sighand->altstack = frame.uc.stack.stack;
        current->sighand->altstack_size = frame.uc.stack.size;
    }
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return cpu->eax;
}

slong_t sys_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct sigframe_ frame;
    // esp points past the first two fields of the frame
    if (user_get(cpu->esp - offsetof(struct sigframe_, sc), frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.sc, cpu);

    lock(&current->sighand->lock);
    sigset_t_ oldmask = ((sigset_t_) frame.extramask << 32) | frame.sc.oldmask;
    sigmask_set(oldmask);
    unlock(&current->sighand->lock);
    return cpu->eax;
}

#endif

struct sighand *sighand_new(void) {
    struct sighand *sighand = malloc(sizeof(struct sighand));
    if (sighand == NULL)
        return NULL;
    memset(sighand, 0, sizeof(struct sighand));
    sighand->refcount = 1;
    lock_init(&sighand->lock);
    return sighand;
}

struct sighand *sighand_copy(struct sighand *sighand) {
    struct sighand *new_sighand = sighand_new();
    if (new_sighand == NULL)
        return NULL;
    memcpy(new_sighand->action, sighand->action, sizeof(new_sighand->action));
    return new_sighand;
}

void sighand_release(struct sighand *sighand) {
    if (--sighand->refcount == 0) {
        free(sighand);
    }
}

static int do_sigaction(int sig, const struct sigaction_ *action, struct sigaction_ *oldaction) {
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (!signal_is_blockable(sig))
        return _EINVAL;

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    if (oldaction)
        *oldaction = sighand->action[sig];
    if (action)
        sighand->action[sig] = *action;
    unlock(&sighand->lock);
    return 0;
}

slong_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, ulong_t sigset_size) {
    if (sigset_size != sizeof(sigset_t_))
        return _EINVAL;
    struct sigaction_ action, oldaction;
    if (action_addr != 0)
        if (user_get(action_addr, action))
            return _EFAULT;
    STRACE("rt_sigaction(%d, %#x {handler=%#x, flags=%#x, restorer=%#x, mask=%#llx}, 0x%x, %d)", signum,
            action_addr, action.handler, action.flags, action.restorer,
            (unsigned long long) action.mask, oldaction_addr, sigset_size);

    int err = do_sigaction(signum,
            action_addr ? &action : NULL,
            oldaction_addr ? &oldaction : NULL);
    if (err < 0)
        return err;

    if (oldaction_addr != 0)
        if (user_put(oldaction_addr, oldaction))
            return _EFAULT;
    return err;
}

slong_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr) {
    return sys_rt_sigaction(signum, action_addr, oldaction_addr, 1);
}

static void sigmask_set(sigset_t_ set) {
    current->blocked = (set & ~UNBLOCKABLE_MASK);
}

static void sigmask_set_temp_unlocked(sigset_t_ mask) {
    current->saved_mask = current->blocked;
    current->has_saved_mask = true;
    sigmask_set(mask);
}

void sigmask_set_temp(sigset_t_ mask) {
    lock(&current->sighand->lock);
    sigmask_set_temp_unlocked(mask);
    unlock(&current->sighand->lock);
}

static int do_sigprocmask(dword_t how, sigset_t_ set) {
    if (how == SIG_BLOCK_)
        sigmask_set(current->blocked | set);
    else if (how == SIG_UNBLOCK_)
        sigmask_set(current->blocked & ~set);
    else if (how == SIG_SETMASK_)
        sigmask_set(set);
    else
        return _EINVAL;
    return 0;
}

slong_t sys_rt_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr, ulong_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;

    sigset_t_ set = 0;
    if (set_addr != 0)
        if (user_get(set_addr, set))
            return _EFAULT;
    STRACE("rt_sigprocmask(%s, %#llx, %#x, %d)",
            how == SIG_BLOCK_ ? "SIG_BLOCK" :
            how == SIG_UNBLOCK_ ? "SIG_UNBLOCK" :
            how == SIG_SETMASK_ ? "SIG_SETMASK" : "??",
            set_addr != 0 ? (long long) set : -1, oldset_addr, size);

    if (oldset_addr != 0)
        if (user_put(oldset_addr, current->blocked))
            return _EFAULT;
    if (set_addr != 0) {
        struct sighand *sighand = current->sighand;
        lock(&sighand->lock);
        int err = do_sigprocmask(how, set);
        unlock(&sighand->lock);
        if (err < 0)
            return err;
    }
    return 0;
}

slong_t sys_rt_sigpending(addr_t set_addr) {
    STRACE("rt_sigpending(%#x)");
    // as defined by the standard
    sigset_t_ pending = current->pending & current->blocked;
    if (user_put(set_addr, pending))
        return _EFAULT;
    return 0;
}

static bool is_on_altstack(addr_t sp, struct sighand *sighand) {
    return sp > sighand->altstack && sp <= sighand->altstack + sighand->altstack_size;
}

static void altstack_to_user(struct sighand *sighand, struct stack_t_ *user_stack) {
    user_stack->stack = sighand->altstack;
    user_stack->size = sighand->altstack_size;
    user_stack->flags = 0;
    if (sighand->altstack == 0)
        user_stack->flags |= SS_DISABLE_;
    if (is_on_altstack(CPU_SP(&current->cpu), sighand))
        user_stack->flags |= SS_ONSTACK_;
}

slong_t sys_sigaltstack(addr_t ss_addr, addr_t old_ss_addr) {
    STRACE("sigaltstack(0x%x, 0x%x)", ss_addr, old_ss_addr);
    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    if (old_ss_addr != 0) {
        struct stack_t_ old_ss;
        altstack_to_user(sighand, &old_ss);
        if (user_put(old_ss_addr, old_ss)) {
            unlock(&sighand->lock);
            return _EFAULT;
        }
    }
    if (ss_addr != 0) {
        if (is_on_altstack(CPU_SP(&current->cpu), sighand)) {
            unlock(&sighand->lock);
            return _EPERM;
        }
        struct stack_t_ ss;
        if (user_get(ss_addr, ss)) {
            unlock(&sighand->lock);
            return _EFAULT;
        }
        if (ss.flags & SS_DISABLE_) {
            sighand->altstack = 0;
        } else {
            if (ss.size < MINSIGSTKSZ_)
                return _ENOMEM;
            sighand->altstack = ss.stack;
            sighand->altstack_size = ss.size;
        }
    }
    unlock(&sighand->lock);
    return 0;
}

slong_t sys_rt_sigsuspend(addr_t mask_addr, ulong_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    STRACE("sigsuspend(0x%llx) = ...\n", (long long) mask);

    lock(&current->sighand->lock);
    sigmask_set_temp_unlocked(mask);
    while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
        continue;
    unlock(&current->sighand->lock);
    STRACE("%d done sigsuspend", current->pid);
    return _EINTR;
}

slong_t sys_pause(void) {
    lock(&current->sighand->lock);
    while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
        continue;
    unlock(&current->sighand->lock);
    return _EINTR;
}

bool dequeue_signal(sigset_t_ mask, struct siginfo_ *info) {
    struct sigqueue *sigqueue;
    list_for_each_entry(&current->queue, sigqueue, queue) {
        int sig = sigqueue->info.sig;
        if (!sigset_has(mask, sig))
            continue;
        list_remove(&sigqueue->queue);
        // no longer pending, or the next one of it would not be queued
        sigset_del(&current->pending, sig);
        *info = sigqueue->info;
        free(sigqueue);
        return true;
    }
    return false;
}

slong_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, ulong_t set_size) {
    if (set_size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ set;
    if (user_get(set_addr, set))
        return _EFAULT;
    struct timespec timeout;
    if (timeout_addr != 0) {
        int err = user_get_timeout(timeout_addr, CLOCK_MONOTONIC_, false, &timeout);
        if (err < 0)
            return err;
    }
    STRACE("sigtimedwait(%#llx, %#x, %#x) = ...\n", (long long) set, info_addr, timeout_addr);

    lock(&current->sighand->lock);
    assert(current->waiting == 0);
    current->waiting = set;
    int err;
    do {
        err = wait_for(&current->pause, &current->sighand->lock, timeout_addr == 0 ? NULL : &timeout);
    } while (err == 0);
    current->waiting = 0;
    if (err == _ETIMEDOUT) {
        unlock(&current->sighand->lock);
        STRACE("sigtimedwait timed out\n");
        return _EAGAIN;
    }

    struct siginfo_ info;
    bool found = dequeue_signal(set, &info);
    unlock(&current->sighand->lock);
    if (!found)
        return _EINTR;
    if (info_addr != 0)
        if (user_put(info_addr, info))
            return _EFAULT;
    STRACE("done sigtimedwait = %d\n", info.sig);
    return info.sig;
}

static int kill_task(struct task *task, dword_t sig) {
    if (!superuser() &&
            current->uid != task->uid &&
            current->uid != task->suid &&
            current->euid != task->uid &&
            current->euid != task->suid)
        return _EPERM;
    struct siginfo_ info = {
        .code = SI_USER_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    send_signal(task, sig, info);
    return 0;
}

static int kill_group(pid_t_ pgid, dword_t sig) {
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct tgroup *tgroup;
    int err = _EPERM;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        int kill_err = kill_task(tgroup->leader, sig);
        // killing a group should return an error only if no process can be signaled
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int kill_everything(dword_t sig) {
    int err = _EPERM;
    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task == NULL || task == current || !task_is_leader(task))
            continue;
        int kill_err = kill_task(task, sig);
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int do_kill(pid_t_ pid, dword_t sig, pid_t_ tgid) {
    STRACE("kill(%d, %d)", pid, sig);
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (pid == 0)
        pid = -current->group->pgid;

    int err;
    lock(&pids_lock);

    if (pid == -1) {
        err = kill_everything(sig);
    } else if (pid < 0) {
        err = kill_group(-pid, sig);
    } else {
        struct task *task = pid_get_task(pid);
        if (task == NULL) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        // If tgid is nonzero, it must be correct
        if (tgid != 0 && task->tgid != tgid) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        err = kill_task(task, sig);
    }

    unlock(&pids_lock);
    return err;
}

slong_t sys_kill(pid_t_ pid, dword_t sig) {
    return do_kill(pid, sig, 0);
}
slong_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig) {
    if (tid <= 0 || tgid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, tgid);
}
slong_t sys_tkill(pid_t_ tid, dword_t sig) {
    if (tid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, 0);
}
