#ifndef EMU_BLINK_GLUE_H
#define EMU_BLINK_GLUE_H

// The seam between the blink engine's two halves. interp.c includes blink's
// headers and nothing of iSH's; engine.c includes iSH's and nothing of
// blink's; this is all either knows of the other, in plain types. The two
// header families define too many of the same names to share a file.

#include <stdbool.h>
#include <stdint.h>
#include "emu/x86_64.h"

struct blink_cpu;

// Why a run came back.
enum blink_stop {
    BLINK_SYSCALL,     // a syscall instruction; rip is past it
    BLINK_FAULT,       // a memory access translate refused: fault_addr, fault_write
    BLINK_UNDEFINED,   // #UD, a decode error or hlt; rip is at the instruction
    BLINK_DIVIDE,      // #DE
    BLINK_FPU,         // an unmasked x87 or SIMD exception
    BLINK_PROTECTION,  // #GP that is not a memory access, or an int n
    BLINK_BREAKPOINT,  // int3; rip is past it
    BLINK_DEBUG,       // int1; rip is past it
    BLINK_TIMER,       // the budget ran out, or the run was poked
};

// Guest address to host pointer, or NULL to fault. write is the access's
// intent; an instruction fetch is a read.
typedef void *(*blink_translate_t)(void *ctx, uint64_t addr, bool write);

struct blink_run {
    // in
    void *ctx;
    blink_translate_t translate;
    uint64_t budget;              // instructions before BLINK_TIMER
    bool *poked;                  // exchanged with false; set means stop
    const uint64_t *changes;      // bumped when any mapping changes
    // out
    uint64_t fault_addr;
    bool fault_write;
};

struct blink_cpu *blink_cpu_new(void);
void blink_cpu_free(struct blink_cpu *cpu);

// Runs from regs until something in enum blink_stop; regs then hold where the
// guest stopped. Call on the thread the guest runs on.
enum blink_stop blink_run(struct blink_cpu *cpu, struct x86_64_regs *regs, struct blink_run *run);

#endif
