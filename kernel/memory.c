#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define DEFAULT_CHANNEL memory
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/memory.h"
#include "emu/engine.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
#include "fs/fd.h"

// increment the change count
static void mem_changed(struct mem *mem);
static struct mmu_ops mem_mmu_ops;

struct pt_dir {
    void *slots[MEM_LEVEL_SIZE]; // struct pt_dir below the last level, struct pt_leaf at it
};
struct pt_leaf {
    unsigned used; // entries with data
    struct pt_entry entries[MEM_LEVEL_SIZE];
};

// The pages one slot covers at a level, counting the root as 0 and the leaf
// as MEM_LEVELS - 1, and a page's index into that level.
#define LEVEL_SHIFT(level) (MEM_LEVEL_BITS * (MEM_LEVELS - 1 - (level)))
#define LEVEL_SPAN(level) ((page_t) 1 << LEVEL_SHIFT(level))
#define LEVEL_INDEX(page, level) (((page) >> LEVEL_SHIFT(level)) & (MEM_LEVEL_SIZE - 1))
#define LEAF_LEVEL (MEM_LEVELS - 1)

void mem_init(struct mem *mem) {
    mem->root = calloc(1, sizeof(struct pt_dir));
    mem->mmu.ops = &mem_mmu_ops;
    engine_mem_init(&mem->mmu);
    mem->mmu.changes = 0;
    wrlock_init(&mem->lock);
}

static void pt_free_level(void *node, int level) {
    if (node == NULL)
        return;
    if (level < LEAF_LEVEL) {
        struct pt_dir *dir = node;
        for (int i = 0; i < MEM_LEVEL_SIZE; i++)
            pt_free_level(dir->slots[i], level + 1);
    }
    free(node);
}

void mem_destroy(struct mem *mem) {
    write_wrlock(&mem->lock);
    pt_unmap_always(mem, 0, MEM_PAGES);
    engine_mem_destroy(&mem->mmu);
    pt_free_level(mem->root, 0);
    write_wrunlock(&mem->lock);
    wrlock_destroy(&mem->lock);
}

// The leaf holding page, made (with any directories above it) if create.
static struct pt_leaf *mem_leaf(struct mem *mem, page_t page, bool create) {
    if (page >= MEM_PAGES)
        return NULL;
    struct pt_dir *dir = mem->root;
    for (int level = 0; level < LEAF_LEVEL; level++) {
        void **slot = &dir->slots[LEVEL_INDEX(page, level)];
        if (*slot == NULL) {
            if (!create)
                return NULL;
            *slot = calloc(1, level + 1 < LEAF_LEVEL ? sizeof(struct pt_dir) : sizeof(struct pt_leaf));
            if (*slot == NULL)
                die("out of memory for the page table");
        }
        dir = *slot;
    }
    return (struct pt_leaf *) dir;
}

static struct pt_entry *mem_pt_new(struct mem *mem, page_t page) {
    struct pt_leaf *leaf = mem_leaf(mem, page, true);
    struct pt_entry *entry = &leaf->entries[LEVEL_INDEX(page, LEAF_LEVEL)];
    // every caller unmaps the page first, and fills in data next
    assert(entry->data == NULL);
    leaf->used++;
    return entry;
}

struct pt_entry *mem_pt(struct mem *mem, page_t page) {
    struct pt_leaf *leaf = mem_leaf(mem, page, false);
    if (leaf == NULL)
        return NULL;
    struct pt_entry *entry = &leaf->entries[LEVEL_INDEX(page, LEAF_LEVEL)];
    if (entry->data == NULL)
        return NULL;
    return entry;
}

static void mem_pt_del(struct mem *mem, page_t page) {
    struct pt_leaf *leaf = mem_leaf(mem, page, false);
    if (leaf == NULL)
        return;
    struct pt_entry *entry = &leaf->entries[LEVEL_INDEX(page, LEAF_LEVEL)];
    if (entry->data != NULL) {
        entry->data = NULL;
        leaf->used--;
    }
}

// Descend towards page. Answers the leaf holding it, or NULL with *level set
// to the level of the missing slot, whose LEVEL_SPAN pages are all unmapped.
static struct pt_leaf *mem_find_leaf(struct mem *mem, page_t page, int *level) {
    struct pt_dir *dir = mem->root;
    for (*level = 0; *level < LEAF_LEVEL; (*level)++) {
        void *child = dir->slots[LEVEL_INDEX(page, *level)];
        if (child == NULL)
            return NULL;
        dir = child;
    }
    return (struct pt_leaf *) dir;
}

page_t mem_next_mapped(struct mem *mem, page_t page) {
    while (page < MEM_PAGES) {
        int level;
        struct pt_leaf *leaf = mem_find_leaf(mem, page, &level);
        if (leaf != NULL && leaf->used != 0) {
            for (unsigned i = LEVEL_INDEX(page, LEAF_LEVEL); i < MEM_LEVEL_SIZE; i++, page++) {
                if (leaf->entries[i].data != NULL)
                    return page;
            }
            continue; // page is now the next leaf's first
        }
        // nothing mapped from here to the end of the missing slot or empty leaf
        page_t span = LEVEL_SPAN(leaf == NULL ? level : LEAF_LEVEL);
        page = (page | (span - 1)) + 1;
    }
    return MEM_PAGES;
}

// The highest page in [floor, page] that is mapped (if mapped) or unmapped
// (if not), or BAD_PAGE if there is none. A leaf whose entries are all the
// other kind, and a missing slot when looking for a mapped page, are stepped
// over whole.
static page_t mem_prev(struct mem *mem, page_t page, page_t floor, bool mapped) {
    while (page != BAD_PAGE && page >= floor) {
        int level;
        struct pt_leaf *leaf = mem_find_leaf(mem, page, &level);
        if (leaf == NULL && !mapped)
            return page;
        page_t span;
        if (leaf == NULL) {
            span = LEVEL_SPAN(level);
        } else if (leaf->used == (mapped ? 0 : MEM_LEVEL_SIZE)) {
            span = LEVEL_SPAN(LEAF_LEVEL);
        } else {
            for (unsigned i = LEVEL_INDEX(page, LEAF_LEVEL); page >= floor; i--, page--) {
                if ((leaf->entries[i].data != NULL) == mapped)
                    return page;
                if (i == 0)
                    break;
            }
            if (page < floor)
                return BAD_PAGE;
            span = LEVEL_SPAN(LEAF_LEVEL);
        }
        // step below the slot or leaf; from the first one, there is nowhere
        page_t base = page & ~(span - 1);
        if (base == 0)
            return BAD_PAGE;
        page = base - 1;
    }
    return BAD_PAGE;
}

// The highest hole of size pages between MEM_HOLE_BOTTOM and MEM_HOLE_TOP,
// placed at its top: the place a page-by-page scan down from the top would
// find first, without scanning.
page_t pt_find_hole(struct mem *mem, pages_t size) {
    if (size == 0)
        return BAD_PAGE;
    page_t top = MEM_HOLE_TOP; // exclusive
    while (top > MEM_HOLE_BOTTOM && top - MEM_HOLE_BOTTOM >= size) {
        page_t free_page = mem_prev(mem, top - 1, MEM_HOLE_BOTTOM, false);
        if (free_page == BAD_PAGE)
            return BAD_PAGE;
        page_t used_page = mem_prev(mem, free_page, MEM_HOLE_BOTTOM, true);
        page_t hole_start = used_page == BAD_PAGE ? MEM_HOLE_BOTTOM : used_page + 1;
        if (free_page + 1 - hole_start >= size)
            return free_page + 1 - size;
        if (used_page == BAD_PAGE)
            return BAD_PAGE;
        top = used_page;
    }
    return BAD_PAGE;
}

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages) {
    return mem_next_mapped(mem, start) >= start + pages;
}

int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags) {
    if (memory == MAP_FAILED)
        return errno_map();

    // If this fails, the munmap in pt_unmap would probably fail.
    assert((uintptr_t) memory % real_page_size == 0 || memory == vdso_data);

    struct data *data = malloc(sizeof(struct data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct data) {
        .data = memory,
        .size = pages * PAGE_SIZE + offset,

#if LEAK_DEBUG
        .pid = current ? current->pid : 0,
        .dest = start << PAGE_BITS,
#endif
    };

    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            pt_unmap(mem, page, 1);
        data->refcount++;
        struct pt_entry *pt = mem_pt_new(mem, page);
        pt->data = data;
        pt->offset = ((page - start) << PAGE_BITS) + offset;
        pt->flags = flags;
    }
    return 0;
}

int pt_unmap(struct mem *mem, page_t start, pages_t pages) {
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return -1;
    return pt_unmap_always(mem, start, pages);
}

int pt_unmap_always(struct mem *mem, page_t start, pages_t pages) {
    for (page_t page = mem_next_mapped(mem, start); page < start + pages; page = mem_next_mapped(mem, page + 1)) {
        struct pt_entry *pt = mem_pt(mem, page);
        engine_page_changed(&mem->mmu, page);
        struct data *data = pt->data;
        mem_pt_del(mem, page);
        if (--data->refcount == 0) {
            // vdso wasn't allocated with mmap, it's just in our data segment
            if (data->data != vdso_data) {
                int err = munmap(data->data, data->size);
                if (err != 0)
                    die("munmap(%p, %lu) failed: %s", data->data, data->size, strerror(errno));
            }
            if (data->fd != NULL) {
                fd_close(data->fd);
            }
            free(data);
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_map_nothing(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    if (pages == 0) return 0;
    void *memory = mmap(NULL, pages * PAGE_SIZE,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    return pt_map(mem, start, pages, memory, 0, flags | P_ANONYMOUS);
}

int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags) {
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return _ENOMEM;
    for (page_t page = start; page < start + pages; page++) {
        struct pt_entry *entry = mem_pt(mem, page);
        int old_flags = entry->flags;
        entry->flags = flags;
        // check if protection is increasing
        if ((flags & ~old_flags) & (P_READ|P_WRITE)) {
            void *data = (char *) entry->data->data + entry->offset;
            // force to be page aligned
            data = (void *) ((uintptr_t) data & ~(real_page_size - 1));
            int prot = PROT_READ;
            if (flags & P_WRITE) prot |= PROT_WRITE;
            if (mprotect(data, real_page_size, prot) < 0)
                return errno_map();
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages) {
    for (page_t page = mem_next_mapped(src, start); page < start + pages; page = mem_next_mapped(src, page + 1)) {
        struct pt_entry *entry = mem_pt(src, page);
        if (pt_unmap_always(dst, page, 1) < 0)
            return -1;
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        struct pt_entry *dst_entry = mem_pt_new(dst, page);
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        dst_entry->flags = entry->flags;
    }
    mem_changed(src);
    mem_changed(dst);
    return 0;
}

static void mem_changed(struct mem *mem) {
    mem->mmu.changes++;
}

// This version will return NULL instead of making necessary pagetable changes.
// Used by the emulator to avoid deadlocks.
static void *mem_ptr_nofault(struct mem *mem, addr_t addr, int type) {
    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry == NULL)
        return NULL;
    if (type == MEM_WRITE && !P_WRITABLE(entry->flags))
        return NULL;
    return entry->data->data + entry->offset + PGOFFSET(addr);
}

void *mem_ptr(struct mem *mem, addr_t addr, int type) {
    void *old_ptr = mem_ptr_nofault(mem, addr, type); // just for an assert

    page_t page = PAGE(addr);
    struct pt_entry *entry = mem_pt(mem, page);

    if (entry == NULL) {
        // page does not exist
        // look to see if the next VM region is willing to grow down
        page_t p = mem_next_mapped(mem, page + 1);
        if (p >= MEM_PAGES)
            return NULL;
        if (!(mem_pt(mem, p)->flags & P_GROWSDOWN))
            return NULL;

        // Changing memory maps must be done with the write lock. But this is
        // called with the read lock.
        // This locking stuff is copy/pasted for all the code in this function
        // which changes memory maps.
        // TODO: factor the lock/unlock code here into a new function. Do this
        // next time you touch this function.
        read_wrunlock(&mem->lock);
        write_wrlock(&mem->lock);
        pt_map_nothing(mem, page, 1, P_WRITE | P_GROWSDOWN);
        write_wrunlock(&mem->lock);
        read_wrlock(&mem->lock);

        entry = mem_pt(mem, page);
    }

    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        // if page is unwritable, well tough luck
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE))
            return NULL;
        if (type == MEM_WRITE_PTRACE) {
            // TODO: Is P_WRITE really correct? The page shouldn't be writable without ptrace.
            entry->flags |= P_WRITE | P_COW;
        }
        // get rid of any compiled blocks in this page
        engine_page_changed(&mem->mmu, page);
        // if page is cow, ~~milk~~ copy it
        if (entry->flags & P_COW) {
            void *data = (char *) entry->data->data + entry->offset;
            void *copy = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

            // copy/paste from above
            read_wrunlock(&mem->lock);
            write_wrlock(&mem->lock);
            memcpy(copy, data, PAGE_SIZE);
            pt_map(mem, page, 1, copy, 0, entry->flags &~ P_COW);
            write_wrunlock(&mem->lock);
            read_wrlock(&mem->lock);
        }
    }

    void *ptr = mem_ptr_nofault(mem, addr, type);
    assert(old_ptr == NULL || old_ptr == ptr || type == MEM_WRITE_PTRACE);
    return ptr;
}

static void *mem_mmu_translate(struct mmu *mmu, addr_t addr, int type) {
    return mem_ptr_nofault(container_of(mmu, struct mem, mmu), addr, type);
}

static struct mmu_ops mem_mmu_ops = {
    .translate = mem_mmu_translate,
};

int mem_segv_reason(struct mem *mem, addr_t addr) {
    struct pt_entry *pt = mem_pt(mem, PAGE(addr));
    if (pt == NULL)
        return SEGV_MAPERR_;
    return SEGV_ACCERR_;
}

size_t real_page_size;
__attribute__((constructor)) static void get_real_page_size() {
    real_page_size = sysconf(_SC_PAGESIZE);
}

void mem_coredump(struct mem *mem, const char *file) {
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return;
    }
    // a sparse file, each page at its own address
    int pages = 0;
    for (page_t page = mem_next_mapped(mem, 0); page < MEM_PAGES; page = mem_next_mapped(mem, page + 1)) {
        struct pt_entry *entry = mem_pt(mem, page);
        pages++;
        if (lseek(fd, (off_t) page << PAGE_BITS, SEEK_SET) < 0) {
            perror("lseek");
            return;
        }
        if (write(fd, entry->data->data, PAGE_SIZE) < 0) {
            perror("write");
            return;
        }
    }
    printk("dumped %d pages\n", pages);
    close(fd);
}
