#ifndef MEMORY_H
#define MEMORY_H

#include <stdatomic.h>
#include <unistd.h>
#include <stdbool.h>
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"
#include "misc.h"

// The page table is a radix tree over page numbers: directories of
// MEM_LEVEL_SIZE slots down to leaves of as many entries. i386's 20-bit page
// numbers are two levels of 10 bits, the shape iSH always had; x86-64's 35
// are four levels of 9. A leaf counts its mapped entries, so a walk can step
// over an empty or a full one without reading it, and nothing below
// MEM_PAGES is ever visited one page at a time.
#if GUEST_X86_64
#define MEM_LEVELS 4
#define MEM_LEVEL_BITS 9
#else
#define MEM_LEVELS 2
#define MEM_LEVEL_BITS 10
#endif
#define MEM_LEVEL_SIZE (1 << MEM_LEVEL_BITS)

struct pt_dir;

struct mem {
    struct pt_dir *root;

    struct mmu mmu;

    wrlock_t lock;
};

// Where things go in a fresh address space. The stack is one page below
// MEM_STACK_TOP that grows down; holes for mmap, the vdso and an interpreter
// are found top-down below MEM_HOLE_TOP (exclusive) and at or above
// MEM_HOLE_BOTTOM, leaving 128 MiB between the two tops; a PIE that has an
// interpreter is placed at MEM_PIE_BIAS. x86-64's are Linux's own with
// randomisation off.
#if GUEST_X86_64
#define MEM_STACK_TOP 0x7ffffffff000
#define MEM_HOLE_TOP 0x7ffff7fff
#define MEM_HOLE_BOTTOM 0x10
#define MEM_PIE_BIAS 0x555555554000
#else
#define MEM_STACK_TOP 0xffffe000
#define MEM_HOLE_TOP 0xf7ffe
#define MEM_HOLE_BOTTOM 0x40001
#define MEM_PIE_BIAS 0x56555000
#endif

// Initialize the address space
void mem_init(struct mem *mem);
// Uninitialize the address space
void mem_destroy(struct mem *mem);
// Return the pagetable entry for the given page
struct pt_entry *mem_pt(struct mem *mem, page_t page);
// The first mapped page at or after page, or MEM_PAGES if there is none.
page_t mem_next_mapped(struct mem *mem, page_t page);

#define BYTES_ROUND_DOWN(bytes) (PAGE(bytes) << PAGE_BITS)
#define BYTES_ROUND_UP(bytes) (PAGE_ROUND_UP(bytes) << PAGE_BITS)

#define LEAK_DEBUG 0

struct data {
    void *data; // immutable
    size_t size; // also immutable
    atomic_uint refcount;

    // for display in /proc/pid/maps
    struct fd *fd;
    size_t file_offset;
    const char *name;
#if LEAK_DEBUG
    int pid;
    addr_t dest;
#endif
};
struct pt_entry {
    struct data *data;
    size_t offset;
    unsigned flags;
    struct list blocks[2];
};
// page flags
// P_READ and P_EXEC are ignored for now
#define P_READ (1 << 0)
#define P_WRITE (1 << 1)
#undef P_EXEC // defined in sys/proc.h on darwin
#define P_EXEC (1 << 2)
#define P_RWX (P_READ | P_WRITE | P_EXEC)
#define P_GROWSDOWN (1 << 3)
#define P_COW (1 << 4)
#define P_WRITABLE(flags) (flags & P_WRITE && !(flags & P_COW))

// mapping was created with pt_map_nothing
#define P_ANONYMOUS (1 << 6)
// mapping was created with MAP_SHARED, should not CoW
#define P_SHARED (1 << 7)

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages);
page_t pt_find_hole(struct mem *mem, pages_t size);

// Map memory + offset into fake memory, unmapping existing mappings. Takes
// ownership of memory. It will be freed with:
// munmap(memory, pages * PAGE_SIZE)
int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags);
// Map empty space into fake memory
int pt_map_nothing(struct mem *mem, page_t page, pages_t pages, unsigned flags);
// Unmap fake memory, return -1 if any part of the range isn't mapped and 0 otherwise
int pt_unmap(struct mem *mem, page_t start, pages_t pages);
// like pt_unmap but doesn't care if part of the range isn't mapped
int pt_unmap_always(struct mem *mem, page_t start, pages_t pages);
// Set the flags on memory
int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags);
// Copy pages from src memory to dst memory using copy-on-write
int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages);

// Must call with mem read-locked.
void *mem_ptr(struct mem *mem, addr_t addr, int type);
int mem_segv_reason(struct mem *mem, addr_t addr);

extern size_t real_page_size;

#endif
