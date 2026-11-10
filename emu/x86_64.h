#ifndef EMU_X86_64_H
#define EMU_X86_64_H

// The x86-64 guest's architectural registers, as the kernel holds them
// between runs of the CPU. Plain types and nothing else included, because
// both sides of the blink engine read it: iSH's kernel, and blink's
// interpreter, whose headers cannot share a translation unit with iSH's.

#include <stdint.h>

struct x86_64_regs {
    union {
        struct {
            uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
            uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
        };
        uint64_t gpr[16]; // in the instruction set's own order
    };
    uint64_t rip;
    uint64_t rflags; // architectural, as pushf would show it
    uint64_t fs_base, gs_base;

    uint32_t mxcsr;
    _Alignas(16) uint8_t xmm[16][16];

    // x87, as blink keeps it: the stack as doubles, not 80-bit extended
    // values. A signal frame converts it to the fxsave layout.
    // The stack top is in sw, bits 11 to 13, as on the hardware.
    struct {
        double st[8];
        uint32_t cw, sw, tw;
    } x87;
};

#endif
