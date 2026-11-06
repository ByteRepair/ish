// The page table, its walks and its hole finder, on the host with no CPU:
// built for each guest, so x86-64's address space is tested before anything
// can run in it. Exits non-zero, saying what, on the first failure.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/memory.h"

static int failures = 0;
#define check(cond, ...) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        failures++; \
    } \
} while (0)

static void map(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    int err = pt_map_nothing(mem, start, pages, P_READ | P_WRITE | flags);
    if (err < 0) {
        printf("FAIL: pt_map_nothing(%#llx, %llu) = %d\n",
                (unsigned long long) start, (unsigned long long) pages, err);
        exit(1);
    }
}

// The hole finder iSH had before the radix table, translated literally: a
// scan down from the top a page at a time. The new one must agree with it.
static page_t scan_for_hole(struct mem *mem, pages_t size) {
    page_t hole_end = 0;
    bool in_hole = false;
    for (page_t page = MEM_HOLE_TOP - 1; page >= MEM_HOLE_BOTTOM; page--) {
        if (!in_hole && mem_pt(mem, page) == NULL) {
            in_hole = true;
            hole_end = page + 1;
        }
        if (mem_pt(mem, page) != NULL)
            in_hole = false;
        else if (hole_end - page == size)
            return page;
    }
    return BAD_PAGE;
}

static void test_empty(void) {
    struct mem mem;
    mem_init(&mem);
    check(mem_next_mapped(&mem, 0) == MEM_PAGES, "an empty space has a mapped page");
    check(pt_find_hole(&mem, 1) == MEM_HOLE_TOP - 1, "an empty space's hole is not at the top");
    check(pt_find_hole(&mem, 0) == BAD_PAGE, "a hole of no pages was found");
    check(mem_pt(&mem, MEM_PAGES) == NULL, "a page past the end has an entry");
    check(mem_pt(&mem, BAD_PAGE) == NULL, "BAD_PAGE has an entry");
    check(pt_is_hole(&mem, 0, MEM_PAGES), "an empty space is not a hole");
    mem_destroy(&mem);
}

static void test_walks(void) {
    struct mem mem;
    mem_init(&mem);
    page_t low = MEM_HOLE_BOTTOM, mid = MEM_PAGES / 2 + 3, high = MEM_PAGES - 1;
    map(&mem, low, 1, 0);
    map(&mem, mid, 2, 0);
    map(&mem, high, 1, 0);
    check(mem_next_mapped(&mem, 0) == low, "first mapped page is not the low one");
    check(mem_next_mapped(&mem, low + 1) == mid, "next after low is not mid");
    check(mem_next_mapped(&mem, mid + 1) == mid + 1, "mid's second page is skipped");
    check(mem_next_mapped(&mem, mid + 2) == high, "next after mid is not the last page");
    check(mem_next_mapped(&mem, high + 1) == MEM_PAGES, "something is mapped past the end");
    check(!pt_is_hole(&mem, mid - 5, 6), "a range ending in a mapped page is a hole");
    check(pt_is_hole(&mem, mid - 5, 5), "the unmapped range below mid is not a hole");

    // unmapping leaves the walk exactly where the mappings are
    pt_unmap(&mem, mid, 1);
    check(mem_next_mapped(&mem, low + 1) == mid + 1, "unmapping mid's first page lost its second");
    pt_unmap_always(&mem, 0, MEM_PAGES);
    check(mem_next_mapped(&mem, 0) == MEM_PAGES, "unmapping everything left a page");
    mem_destroy(&mem);
}

static void test_holes(void) {
    struct mem mem;
    mem_init(&mem);
    page_t top = MEM_HOLE_TOP;
    map(&mem, top - 10, 10, 0);
    check(pt_find_hole(&mem, 5) == top - 15, "the hole is not just below the top mapping");
    // a gap of 3 between two mappings: a 3 fits it, a 4 goes below
    map(&mem, top - 20, 7, 0);
    check(pt_find_hole(&mem, 3) == top - 13, "a 3-page hole does not fill the 3-page gap");
    check(pt_find_hole(&mem, 4) == top - 24, "a 4-page hole does not go below the gap");

    // several whole leaves mapped below: the finder steps over them
    page_t full = (pages_t) MEM_LEVEL_SIZE * 3;
    map(&mem, top - 20 - full, full, 0);
    check(pt_find_hole(&mem, 4) == top - 24 - full, "the hole does not skip the full leaves");
    mem_destroy(&mem);
}

// Fixed layouts within a few leaves of the top: mappings of assorted sizes
// with gaps between, and holes of assorted sizes asked of each. The same on
// every run: the sequence comes from a fixed seed.
static void test_holes_against_scan(void) {
    unsigned state = 12345;
    #define NEXT() (state = state * 1103515245 + 12345, (state >> 16) & 0x7fff)
    for (int layout = 0; layout < 40; layout++) {
        struct mem mem;
        mem_init(&mem);
        page_t page = MEM_HOLE_TOP - 1 - NEXT() % 8;
        for (int i = 0; i < 12; i++) {
            pages_t len = 1 + NEXT() % (MEM_LEVEL_SIZE + 40);
            if (page < MEM_HOLE_BOTTOM + len + 200)
                break;
            page -= len;
            map(&mem, page, len, 0);
            page -= NEXT() % 24; // the gap below, sometimes none
        }
        for (pages_t size = 1; size <= 40; size += 1 + size / 4) {
            page_t want = scan_for_hole(&mem, size);
            page_t got = pt_find_hole(&mem, size);
            check(got == want, "layout %d, size %llu: found %#llx, the scan finds %#llx", layout,
                    (unsigned long long) size, (unsigned long long) got, (unsigned long long) want);
        }
        mem_destroy(&mem);
    }
}

static void test_copy_on_write(void) {
    struct mem src, dst;
    mem_init(&src);
    mem_init(&dst);
    page_t page = MEM_HOLE_TOP - 1;
    addr_t addr = (addr_t) page << PAGE_BITS;
    map(&src, page, 1, 0);

    read_wrlock(&src.lock);
    strcpy(mem_ptr(&src, addr, MEM_WRITE), "parent");
    read_wrunlock(&src.lock);

    pt_copy_on_write(&src, &dst, 0, MEM_PAGES);
    check(mem_pt(&dst, page) != NULL, "the copy has no page");
    check(mem_pt(&src, page)->flags & P_COW, "the source page is not CoW");

    read_wrlock(&dst.lock);
    check(strcmp(mem_ptr(&dst, addr, MEM_READ), "parent") == 0, "the copy does not read the parent's data");
    strcpy(mem_ptr(&dst, addr, MEM_WRITE), "child");
    read_wrunlock(&dst.lock);

    read_wrlock(&src.lock);
    check(strcmp(mem_ptr(&src, addr, MEM_READ), "parent") == 0, "writing the copy changed the parent");
    read_wrunlock(&src.lock);
    read_wrlock(&dst.lock);
    check(strcmp(mem_ptr(&dst, addr, MEM_READ), "child") == 0, "the copy lost its own write");
    read_wrunlock(&dst.lock);

    mem_destroy(&src);
    mem_destroy(&dst);
}

static void test_stack_growth(void) {
    struct mem mem;
    mem_init(&mem);
    page_t stack = PAGE(MEM_STACK_TOP) - 1;
    map(&mem, stack, 1, P_GROWSDOWN);
    // a fault a few leaves below finds the stack above it and grows there
    addr_t below = ((addr_t) stack - 3) << PAGE_BITS;
    read_wrlock(&mem.lock);
    check(mem_ptr(&mem, below, MEM_WRITE) != NULL, "the stack did not grow down");
    read_wrunlock(&mem.lock);
    check(mem_pt(&mem, stack - 3) != NULL, "the grown page is not mapped");
    // nothing above an ordinary mapping grows into it
    map(&mem, MEM_HOLE_BOTTOM + 10, 1, 0);
    read_wrlock(&mem.lock);
    check(mem_ptr(&mem, (addr_t) (MEM_HOLE_BOTTOM + 9) << PAGE_BITS, MEM_WRITE) == NULL,
            "a mapping that does not grow down grew");
    read_wrunlock(&mem.lock);
    mem_destroy(&mem);
}

int main(void) {
    test_empty();
    test_walks();
    test_holes();
    test_holes_against_scan();
    test_copy_on_write();
    test_stack_growth();
    if (failures)
        return 1;
    printf("ok: %d-level table, %#llx pages\n", MEM_LEVELS, (unsigned long long) MEM_PAGES);
    return 0;
}
