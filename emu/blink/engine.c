// The iSH half of the blink engine: cpu_run_to_interrupt over blink's
// interpreter (interp.c, through glue.h), and the memory hooks emu/engine.h
// asks of an engine. Memory is iSH's: a guest access translates through
// iSH's TLB, and a miss through mem_ptr, which faults in what Linux would —
// a copy-on-write copy, a page of stack — so the only faults that reach the
// kernel are the ones that end in SIGSEGV. blink's faults cannot be
// restarted (an instruction may have moved RSP before it touched memory),
// which is why the work is done in the translate rather than after it.
#include "debug.h"
#include "emu/cpu.h"
#include "emu/engine.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"
#include "kernel/memory.h"
#include "emu/blink/glue.h"

void engine_cpu_copied(struct cpu_state *cpu) {
    cpu->engine = NULL;
}

void engine_cpu_destroy(struct cpu_state *cpu) {
    blink_cpu_free(cpu->engine);
    cpu->engine = NULL;
}

struct translate_ctx {
    struct tlb *tlb;
    struct mem *mem;
};

static void *translate(void *opaque, uint64_t addr, bool write) {
    struct translate_ctx *ctx = opaque;
    if (addr >= (uint64_t) MEM_PAGES << PAGE_BITS)
        return NULL;
    void *ptr = write ? __tlb_write_ptr(ctx->tlb, addr) : __tlb_read_ptr(ctx->tlb, addr);
    if (ptr != NULL)
        return ptr;
    // The TLB's own translate does not fault anything in. mem_ptr does, with
    // the mem read-locked, as task_run_current holds it around the run.
    ptr = mem_ptr(ctx->mem, addr, write ? MEM_WRITE : MEM_READ);
    // A copy-on-write copy or a new stack page changed the mappings, and a
    // read entry for the copied page would still point at the shared one.
    if (ctx->tlb->mem_changes != ctx->tlb->mmu->changes)
        tlb_flush(ctx->tlb);
    return ptr;
}

// Instructions per run before the kernel gets a look in, the way asbestos
// comes back every 1024 blocks: pending signals are delivered there.
#define RUN_BUDGET (1 << 14)

int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    if (cpu->poked_ptr == NULL)
        cpu->poked_ptr = &cpu->_poked;
    if (cpu->engine == NULL) {
        cpu->engine = blink_cpu_new();
        if (cpu->engine == NULL)
            die("could not allocate a blink cpu");
    }
    tlb_refresh(tlb, cpu->mmu);

    struct translate_ctx ctx = {
        .tlb = tlb,
        .mem = container_of(cpu->mmu, struct mem, mmu),
    };
    struct blink_run run = {
        .ctx = &ctx,
        .translate = translate,
        .budget = RUN_BUDGET,
        .poked = cpu->poked_ptr,
        .changes = &cpu->mmu->changes,
    };

    int interrupt;
    switch (blink_run(cpu->engine, &cpu->r, &run)) {
        case BLINK_SYSCALL: interrupt = INT_SYSCALL; break;
        case BLINK_FAULT:
            cpu->segfault_addr = run.fault_addr;
            cpu->segfault_was_write = run.fault_write;
            interrupt = INT_GPF;
            break;
        case BLINK_UNDEFINED: interrupt = INT_UNDEFINED; break;
        case BLINK_DIVIDE: interrupt = INT_DIV; break;
        case BLINK_FPU: interrupt = INT_FPU; break;
        case BLINK_PROTECTION:
            // no address to fault in: the kernel's mem_ptr of page zero
            // answers NULL, and the process gets its SIGSEGV
            cpu->segfault_addr = 0;
            cpu->segfault_was_write = false;
            interrupt = INT_GPF;
            break;
        case BLINK_BREAKPOINT: interrupt = INT_BREAKPOINT; break;
        case BLINK_DEBUG: interrupt = INT_DEBUG; break;
        case BLINK_TIMER: interrupt = INT_TIMER; break;
        default: die("blink stopped for no reason it knows");
    }
    cpu->trapno = interrupt;
    return interrupt;
}

void cpu_poke(struct cpu_state *cpu) {
    __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
}
