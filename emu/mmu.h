#ifndef EMU_CPU_MEM_H
#define EMU_CPU_MEM_H

#include "misc.h"

// an address >> 12: 20 bits of an i386 address, 35 of an x86-64 one
typedef ulong_t page_t;
// No page is this one: all ones is past the top of every guest's address
// space. (It was 0x10000, a real page that only the hole finder's range kept
// from coming back.)
#define BAD_PAGE ((page_t) -1)

#ifndef __KERNEL__
#define PAGE_BITS 12
#undef PAGE_SIZE // defined in system headers somewhere
#define PAGE_SIZE (1 << PAGE_BITS)
#define PAGE(addr) ((addr) >> PAGE_BITS)
#define PGOFFSET(addr) ((addr) & (PAGE_SIZE - 1))
typedef ulong_t pages_t;
// bytes MUST be unsigned if you would like this to overflow to zero
#define PAGE_ROUND_UP(bytes) (PAGE((bytes) + PAGE_SIZE - 1))
#if GUEST_X86_64
#define MEM_PAGES ((page_t) 1 << 35) // the 47-bit user half
#else
#define MEM_PAGES (1 << 20)
#endif
#endif

struct mmu {
    struct mmu_ops *ops;
    // the engine's own state for this address space; see emu/engine.h
#if !ENGINE_BLINK
    struct asbestos *asbestos;
#endif
    uint64_t changes;
};

#define MEM_READ 0
#define MEM_WRITE 1
#define MEM_WRITE_PTRACE 2

struct mmu_ops {
    // type is MEM_READ or MEM_WRITE
    void *(*translate)(struct mmu *mmu, addr_t addr, int type);
};

static inline void *mmu_translate(struct mmu *mmu, addr_t addr, int type) {
    return mmu->ops->translate(mmu, addr, type);
}

#endif