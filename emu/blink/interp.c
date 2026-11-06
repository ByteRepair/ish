// The blink half of the engine: blink's interpreter run as a CPU and
// nothing else. iSH's kernel owns memory, processes, signals and syscalls,
// so this file supplies what blink's own kernel side would have — the
// address translation (to iSH's page tables, through the embedder), the
// halt path (back to the caller rather than to a signal), the syscall
// instruction (a stop, not a call) — and the rest of blink stays out of the
// build. It includes blink's headers and none of iSH's; glue.h is the seam.
#include <errno.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "blink/bus.h"
#include "blink/endian.h"
#include "blink/flags.h"
#include "blink/linux.h"
#include "blink/machine.h"
#include "blink/signal.h"
#include "blink/syscall.h"
#include "blink/xlat.h"
#include "emu/blink/glue.h"

struct blink_cpu {
    struct Machine *m;
    uint64_t changes_seen;
};

// The run in progress on this thread, for the hooks blink calls without a
// way to pass it.
static _Thread_local struct blink_run *t_run;
static _Thread_local enum blink_stop t_stop;
static _Thread_local bool t_stopped;

// One System for every Machine: blink reads only its mode here, since the
// page tables, fds, signal handlers and the rest it would hold are iSH's.
static struct System g_system;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void init_once(void) {
    g_system.mode = XED_MACHINE_MODE_LONG;
    InitBus();
}

struct blink_cpu *blink_cpu_new(void) {
    pthread_once(&g_once, init_once);
    struct blink_cpu *cpu = calloc(1, sizeof(*cpu));
    if (cpu == NULL)
        return NULL;
    struct Machine *m;
    if (posix_memalign((void **) &m, _Alignof(struct Machine), sizeof(*m))) {
        free(cpu);
        return NULL;
    }
    memset(m, 0, sizeof(*m));
    ResetCpu(m);
    m->system = &g_system;
    m->mode = g_system.mode;
    cpu->m = m;
    cpu->changes_seen = UINT64_MAX;
    return cpu;
}

void blink_cpu_free(struct blink_cpu *cpu) {
    if (cpu == NULL)
        return;
    free(cpu->m);
    free(cpu);
}

// Every guest access resolves here (BLINK_EXTERNAL_MMU): a write asks with
// PAGE_RW in need, a fetch and a read without.
u8 *LookupAddress2(struct Machine *m, i64 virt, u64 mask, u64 need) {
    (void) mask;
    struct blink_run *run = t_run;
    bool write = (need & PAGE_RW) != 0;
    u8 *host = run->translate(run->ctx, (u64) virt, write);
    if (host == NULL) {
        run->fault_write = write;
        m->segvcode = SEGV_MAPERR_LINUX;
        errno = EFAULT;
    }
    return host;
}

// blink's throw.c, less the signal delivery: the kernel decides what a
// fault means for the process, so a halt only records it and unwinds to
// blink_run.
void RestoreIp(struct Machine *m) {
    if (m) {
        m->ip -= m->oplen;
        m->oplen = 0;
    }
}

static _Noreturn void halt(struct Machine *m, enum blink_stop stop) {
    t_stop = stop;
    t_stopped = true;
    // a cross-page store stashed before the fault must not be committed by
    // the next instruction
    m->stashaddr = 0;
    siglongjmp(m->onhalt, 1);
}

void HaltMachine(struct Machine *m, int code) {
    m->trapno = code;
    switch (code) {
        case kMachineSegmentationFault:
            RestoreIp(m);
            t_run->fault_addr = (u64) m->faultaddr;
            halt(m, BLINK_FAULT);
        case kMachineDivideError:
            RestoreIp(m);
            halt(m, BLINK_DIVIDE);
        case kMachineFpuException:
        case kMachineSimdException:
            RestoreIp(m);
            halt(m, BLINK_FPU);
        case kMachineHalt:
        case kMachineDecodeError:
        case kMachineUndefinedInstruction:
            RestoreIp(m);
            halt(m, BLINK_UNDEFINED);
        case 1:
            // traps: rip stays past the instruction, as on the hardware
            m->oplen = 0;
            halt(m, BLINK_DEBUG);
        case 3:
            m->oplen = 0;
            halt(m, BLINK_BREAKPOINT);
        default:
            // a protection fault, or an int n the guest has no business
            // making in long mode (int 0x80 included): SIGSEGV either way
            RestoreIp(m);
            halt(m, BLINK_PROTECTION);
    }
}

void RaiseDivideError(struct Machine *m) {
    HaltMachine(m, kMachineDivideError);
}

void ThrowProtectionFault(struct Machine *m) {
    HaltMachine(m, kMachineProtectionFault);
}

void ThrowSegmentationFault(struct Machine *m, i64 va) {
    RestoreIp(m);
    m->faultaddr = va;
    HaltMachine(m, kMachineSegmentationFault);
}

void OpUdImpl(struct Machine *m) {
    RestoreIp(m);
    HaltMachine(m, kMachineUndefinedInstruction);
}

void OpUd(P) {
    OpUdImpl(m);
}

void OpHlt(P) {
    HaltMachine(m, kMachineHalt);
}

// The syscall instruction ends the run; the kernel answers in rax. rcx and
// r11 take rip and rflags, as the hardware's SYSCALL does.
void OpSyscall(P) {
    Put64(m->cx, m->ip);
    Put64(m->r11, ExportFlags(m->flags) & 0x3fffff);
    t_stop = BLINK_SYSCALL;
    t_stopped = true;
}

static void load(struct Machine *m, const struct x86_64_regs *r) {
    for (int i = 0; i < 16; i++)
        Put64(m->weg[i], r->gpr[i]);
    m->ip = r->rip;
    ImportFlags(m, r->rflags);
    m->fs.base = r->fs_base;
    m->gs.base = r->gs_base;
    m->mxcsr = r->mxcsr;
    memcpy(m->xmm, r->xmm, sizeof(m->xmm));
    memcpy(m->fpu.st, r->x87.st, sizeof(m->fpu.st));
    m->fpu.cw = r->x87.cw;
    m->fpu.sw = r->x87.sw;
    m->fpu.tw = r->x87.tw;
}

static void store(const struct Machine *m, struct x86_64_regs *r) {
    for (int i = 0; i < 16; i++)
        r->gpr[i] = Get64(m->weg[i]);
    r->rip = m->ip;
    // bits 22 and up are reserved, and blink keeps its lazy parity there
    r->rflags = ExportFlags(m->flags) & 0x3fffff;
    r->fs_base = m->fs.base;
    r->gs_base = m->gs.base;
    r->mxcsr = m->mxcsr;
    memcpy(r->xmm, m->xmm, sizeof(m->xmm));
    memcpy(r->x87.st, m->fpu.st, sizeof(m->fpu.st));
    r->x87.cw = m->fpu.cw;
    r->x87.sw = m->fpu.sw;
    r->x87.tw = m->fpu.tw;
}

// How often the poke flag and the mapping counter are looked at.
#define CHECK_EVERY 64

enum blink_stop blink_run(struct blink_cpu *cpu, struct x86_64_regs *regs, struct blink_run *run) {
    struct Machine *m = cpu->m;
    load(m, regs);
    g_machine = m;
    t_run = run;
    t_stopped = false;
    m->oplen = 0;
    m->canhalt = true;

    if (sigsetjmp(m->onhalt, 0) == 0) {
        for (uint64_t done = 0; !t_stopped; done++) {
            if (done % CHECK_EVERY == 0) {
                // a mapping changed: the cached code page may be stale
                uint64_t changes = __atomic_load_n(run->changes, __ATOMIC_ACQUIRE);
                if (changes != cpu->changes_seen) {
                    ResetInstructionCache(m);
                    cpu->changes_seen = changes;
                }
                if (done >= run->budget || __atomic_exchange_n(run->poked, false, __ATOMIC_SEQ_CST)) {
                    t_stop = BLINK_TIMER;
                    break;
                }
            }
            ExecuteInstruction(m);
        }
    }

    m->canhalt = false;
    store(m, regs);
    t_run = NULL;
    return t_stop;
}

// What the dropped kernel side supplied. InitBus allocates its LOCK stripes
// with these; the rest are reached only from blink's own page-table walk and
// its own run loop (Blink, Actor), which nothing here calls, and abort if
// that ever changes.
void *AllocateBig(size_t n, int prot, int flags, int fd, off_t off) {
    void *p = mmap(0, n, prot, flags, fd, off);
    return p != MAP_FAILED ? p : 0;
}

void FreeBig(void *p, size_t n) {
    if (p)
        munmap(p, n);
}

struct HostPages g_hostpages;

#define UNREACHABLE(what) do { \
    (void) what; \
    abort(); \
} while (0)

u64 AllocateAnonymousPage(struct System *s) {
    UNREACHABLE(s);
}

void FreeAnonymousPage(struct System *s, u8 *page) {
    UNREACHABLE(s);
    (void) page;
}

void CheckForSignals(struct Machine *m) {
    UNREACHABLE(m);
}

void CollectGarbage(struct Machine *m, size_t mark) {
    UNREACHABLE(m);
    (void) mark;
}

i64 ConvertHostToGuestAddress(struct System *s, void *ha, u64 *out_pte) {
    UNREACHABLE(s);
    (void) ha, (void) out_pte;
}

void DeliverSignalToUser(struct Machine *m, int sig, int code) {
    UNREACHABLE(m);
    (void) sig, (void) code;
}

int UnXlatSiCode(int sig, int code) {
    UNREACHABLE(sig);
    (void) code;
}

int UnXlatSignal(int sig) {
    UNREACHABLE(sig);
}

void UnlockRobustFutexes(struct Machine *m) {
    (void) m;
}
