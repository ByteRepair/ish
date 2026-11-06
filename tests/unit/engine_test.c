// The blink engine on raw x86-64, with no loader and no kernel: each test
// maps memory, writes hand-assembled instructions into it, runs them through
// cpu_run_to_interrupt and checks how the run came back, the registers, and
// the memory. Built only for the x86_64 guest.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu/cpu.h"
#include "emu/engine.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"
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

#define CODE 0x400000ull
#define DATA 0x600000ull
#define STACK_TOP 0x800000ull

static void map(struct mem *mem, uint64_t addr, unsigned pages, unsigned flags) {
    if (pt_map_nothing(mem, PAGE(addr), pages, P_READ | flags) < 0) {
        printf("FAIL: could not map %#llx\n", (unsigned long long) addr);
        exit(1);
    }
}

// Write into guest memory as the kernel would, faulting in what needs it.
static void poke(struct mem *mem, uint64_t addr, const void *bytes, size_t n) {
    read_wrlock(&mem->lock);
    for (size_t i = 0; i < n; i++)
        *(uint8_t *) mem_ptr(mem, addr + i, MEM_WRITE_PTRACE) = ((const uint8_t *) bytes)[i];
    read_wrunlock(&mem->lock);
}

static uint64_t peek64(struct mem *mem, uint64_t addr) {
    uint64_t value;
    read_wrlock(&mem->lock);
    memcpy(&value, mem_ptr(mem, addr, MEM_READ), sizeof(value));
    read_wrunlock(&mem->lock);
    return value;
}

static void setup(struct mem *mem, struct cpu_state *cpu, const void *code, size_t n) {
    mem_init(mem);
    map(mem, CODE, 1, 0);
    poke(mem, CODE, code, n);
    // poke made the code page writable, as ptrace's write would; take that back
    pt_set_flags(mem, PAGE(CODE), 1, P_READ);
    *cpu = (struct cpu_state) {
        .mmu = &mem->mmu,
        .r = {
            .rip = CODE,
            .rsp = STACK_TOP,
            .rflags = 0x2,
            .mxcsr = 0x1f80,
            .x87 = {.cw = 0x37f},
        },
    };
}

static int run(struct mem *mem, struct cpu_state *cpu) {
    struct tlb tlb = {};
    read_wrlock(&mem->lock);
    int interrupt = cpu_run_to_interrupt(cpu, &tlb);
    read_wrunlock(&mem->lock);
    return interrupt;
}

static void teardown(struct mem *mem, struct cpu_state *cpu) {
    engine_cpu_destroy(cpu);
    mem_destroy(mem);
}

// mov eax, 60; mov edi, 7; syscall
static void test_syscall(void) {
    static const uint8_t code[] = {0xb8, 60, 0, 0, 0, 0xbf, 7, 0, 0, 0, 0x0f, 0x05};
    struct mem mem;
    struct cpu_state cpu;
    setup(&mem, &cpu, code, sizeof(code));
    check(run(&mem, &cpu) == INT_SYSCALL, "syscall did not stop the run");
    check(cpu.r.rax == 60 && cpu.r.rdi == 7, "syscall arguments: rax %llu rdi %llu",
            (unsigned long long) cpu.r.rax, (unsigned long long) cpu.r.rdi);
    check(cpu.r.rip == CODE + sizeof(code), "rip is not past the syscall");
    check(cpu.r.rcx == cpu.r.rip, "rcx does not hold the return address");
    // the kernel answers in rax and the guest carries on from rip
    cpu.r.rax = 0;
    teardown(&mem, &cpu);
}

// mov rax, imm64; push rax; pop rbx; stc; syscall
static void test_stack_and_flags(void) {
    static const uint8_t code[] = {
        0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x50, 0x5b, 0xf9, 0x0f, 0x05,
    };
    struct mem mem;
    struct cpu_state cpu;
    setup(&mem, &cpu, code, sizeof(code));
    map(&mem, STACK_TOP - 0x1000, 1, P_WRITE | P_GROWSDOWN);
    check(run(&mem, &cpu) == INT_SYSCALL, "the run did not reach its syscall");
    check(cpu.r.rbx == 0x1122334455667788ull, "pop did not return what push pushed");
    check(cpu.r.rsp == STACK_TOP, "push and pop did not balance");
    check(peek64(&mem, STACK_TOP - 8) == 0x1122334455667788ull, "the pushed value is not in memory");
    check(cpu.r.rflags & 1, "stc did not set CF");
    check(cpu.r.r11 == cpu.r.rflags, "r11 does not hold rflags");
    teardown(&mem, &cpu);
}

// Through each store path the write-intent patches cover, on pages shared
// copy-on-write with another address space: the parent's must not change,
// and a read after the write must see the child's copy. Each store has a
// page to itself, so none rides on a copy another store already made.
//   mov rbx, [rdi]; mov [rdi], rax; mov rdx, [rdi]   (ModR/M, page 0)
//   mov [abs DATA+0x1008], rax                        (moffs, page 1)
//   push rax onto a CoW stack page                    (PushN)
//   lea rdi, [rdi+0x2000]; mov ecx, 16; mov al, 0x55;
//   rep stosb                                         (string, page 2)
//   movdqa [rsi], xmm0 with rsi = DATA+0x3000         (vector, page 3)
//   syscall
static void test_copy_on_write(void) {
    static const uint8_t code[] = {
        0x48, 0x8b, 0x1f,                         // mov rbx, [rdi]
        0x48, 0x89, 0x07,                         // mov [rdi], rax
        0x48, 0x8b, 0x17,                         // mov rdx, [rdi]
        0x48, 0xa3, 0x08, 0x10, 0x60, 0, 0, 0, 0, 0,  // mov [0x601008], rax
        0x50,                                     // push rax
        0x48, 0x8d, 0xbf, 0x00, 0x20, 0, 0,       // lea rdi, [rdi+0x2000]
        0xb9, 16, 0, 0, 0,                        // mov ecx, 16
        0xb0, 0x55,                               // mov al, 0x55
        0xf3, 0xaa,                               // rep stosb
        0x66, 0x0f, 0x7f, 0x06,                   // movdqa [rsi], xmm0
        0x0f, 0x05,                               // syscall
    };
    struct mem parent, child;
    struct cpu_state cpu;
    setup(&parent, &cpu, code, sizeof(code));
    map(&parent, DATA, 4, P_WRITE);
    map(&parent, STACK_TOP - 0x1000, 1, P_WRITE | P_GROWSDOWN);
    uint64_t old = 0xaaaaaaaaaaaaaaaaull, old_stack = 0xbbbbbbbbbbbbbbbbull;
    for (int page = 0; page < 4; page++)
        for (int i = 0; i < 0x20; i += 8)
            poke(&parent, DATA + page * 0x1000 + i, &old, 8);
    poke(&parent, STACK_TOP - 8, &old_stack, 8);

    mem_init(&child);
    pt_copy_on_write(&parent, &child, 0, MEM_PAGES);
    cpu.mmu = &child.mmu;
    cpu.r.rax = 0x1234;
    cpu.r.rdi = DATA;
    cpu.r.rsi = DATA + 0x3000;
    memset(cpu.r.xmm[0], 0x77, 16);

    check(run(&child, &cpu) == INT_SYSCALL, "the run did not reach its syscall");
    check(cpu.r.rbx == old, "the first read did not see the shared page");
    check(cpu.r.rdx == 0x1234, "the read after the write saw %#llx, not the child's copy",
            (unsigned long long) cpu.r.rdx);
    check(peek64(&child, DATA) == 0x1234, "the child's ModR/M store is not in its memory");
    check(peek64(&child, DATA + 0x1008) == 0x1234, "the child's moffs store is not in its memory");
    check(peek64(&child, STACK_TOP - 8) == 0x1234, "the child's push is not on its stack");
    check(peek64(&child, DATA + 0x2000) == 0x5555555555555555ull, "the child's rep stosb is not in its memory");
    check(peek64(&child, DATA + 0x3000) == 0x7777777777777777ull, "the child's movdqa store is not in its memory");

    static const char *const path[] = {"ModR/M", "moffs", "rep stosb", "movdqa"};
    for (int page = 0; page < 4; page++)
        for (int i = 0; i < 0x20; i += 8)
            check(peek64(&parent, DATA + page * 0x1000 + i) == old,
                    "the %s store changed the parent's page at +%#x", path[page], i);
    check(peek64(&parent, STACK_TOP - 8) == old_stack, "the parent's stack changed");

    engine_cpu_destroy(&cpu);
    mem_destroy(&child);
    mem_destroy(&parent);
}

// lock cmpxchg16b [rdi]; syscall, with rdx:rax matching the zeros there:
// it stores rcx:rbx, which on a CoW page must copy it first.
static void test_cmpxchg16b(void) {
    static const uint8_t code[] = {0xf0, 0x48, 0x0f, 0xc7, 0x0f, 0x0f, 0x05};
    struct mem parent, child;
    struct cpu_state cpu;
    setup(&parent, &cpu, code, sizeof(code));
    map(&parent, DATA, 1, P_WRITE);
    mem_init(&child);
    pt_copy_on_write(&parent, &child, 0, MEM_PAGES);
    cpu.mmu = &child.mmu;
    cpu.r.rdi = DATA;
    cpu.r.rbx = 7;
    cpu.r.rcx = 9;
    check(run(&child, &cpu) == INT_SYSCALL, "the run did not reach its syscall");
    check(cpu.r.rflags & (1 << 6), "cmpxchg16b did not match");
    check(peek64(&child, DATA) == 7 && peek64(&child, DATA + 8) == 9, "cmpxchg16b's store is not in the child");
    check(peek64(&parent, DATA) == 0 && peek64(&parent, DATA + 8) == 0, "cmpxchg16b changed the parent's page");
    engine_cpu_destroy(&cpu);
    mem_destroy(&child);
    mem_destroy(&parent);
}

// A read of an unmapped page, then a write to a read-only one: each stops at
// its instruction with the address and the intent, and nothing has moved.
static void test_faults(void) {
    static const uint8_t code[] = {0x48, 0x8b, 0x07, 0x0f, 0x05};  // mov rax, [rdi]; syscall
    struct mem mem;
    struct cpu_state cpu;
    setup(&mem, &cpu, code, sizeof(code));
    cpu.r.rdi = 0x10000000;
    check(run(&mem, &cpu) == INT_GPF, "a read of nothing did not fault");
    check(cpu.segfault_addr == 0x10000000 && !cpu.segfault_was_write, "read fault: addr %#llx write %d",
            (unsigned long long) cpu.segfault_addr, cpu.segfault_was_write);
    check(cpu.r.rip == CODE, "the faulting read's rip moved");
    teardown(&mem, &cpu);

    static const uint8_t store[] = {0x48, 0x89, 0x07, 0x0f, 0x05};  // mov [rdi], rax; syscall
    setup(&mem, &cpu, store, sizeof(store));
    map(&mem, DATA, 1, 0);
    cpu.r.rdi = DATA;
    check(run(&mem, &cpu) == INT_GPF, "a write to a read-only page did not fault");
    check(cpu.segfault_addr == DATA && cpu.segfault_was_write, "write fault: addr %#llx write %d",
            (unsigned long long) cpu.segfault_addr, cpu.segfault_was_write);
    // and once the kernel has made it writable, the same instruction runs
    pt_set_flags(&mem, PAGE(DATA), 1, P_READ | P_WRITE);
    cpu.r.rax = 99;
    check(run(&mem, &cpu) == INT_SYSCALL, "the store did not run again after the fix");
    check(peek64(&mem, DATA) == 99, "the restarted store did not land");
    teardown(&mem, &cpu);
}

// A push that faults leaves rsp where it was; one into a page below a stack
// that grows down grows it instead.
static void test_push(void) {
    static const uint8_t code[] = {0x50, 0x0f, 0x05};  // push rax; syscall
    struct mem mem;
    struct cpu_state cpu;
    setup(&mem, &cpu, code, sizeof(code));
    cpu.r.rsp = 0x20000000;
    check(run(&mem, &cpu) == INT_GPF, "a push to nothing did not fault");
    check(cpu.r.rsp == 0x20000000, "the faulting push moved rsp to %#llx", (unsigned long long) cpu.r.rsp);
    check(cpu.r.rip == CODE, "the faulting push's rip moved");
    teardown(&mem, &cpu);

    setup(&mem, &cpu, code, sizeof(code));
    map(&mem, STACK_TOP - 0x1000, 1, P_WRITE | P_GROWSDOWN);
    cpu.r.rsp = STACK_TOP - 0x1000;  // the next push lands a page below
    cpu.r.rax = 5;
    check(run(&mem, &cpu) == INT_SYSCALL, "a push below the stack did not grow it");
    check(peek64(&mem, STACK_TOP - 0x1008) == 5, "the push did not land in the grown page");
    teardown(&mem, &cpu);
}

static void test_traps(void) {
    struct mem mem;
    struct cpu_state cpu;
    static const uint8_t ud2[] = {0x0f, 0x0b};
    setup(&mem, &cpu, ud2, sizeof(ud2));
    check(run(&mem, &cpu) == INT_UNDEFINED, "ud2 was not undefined");
    check(cpu.r.rip == CODE, "ud2's rip moved");
    teardown(&mem, &cpu);

    static const uint8_t div0[] = {0x31, 0xc9, 0x48, 0xf7, 0xf1};  // xor ecx, ecx; div rcx
    setup(&mem, &cpu, div0, sizeof(div0));
    check(run(&mem, &cpu) == INT_DIV, "dividing by zero did not trap");
    check(cpu.r.rip == CODE + 2, "the divide's rip is not at the div");
    teardown(&mem, &cpu);

    static const uint8_t spin[] = {0xeb, 0xfe};  // jmp $
    setup(&mem, &cpu, spin, sizeof(spin));
    check(run(&mem, &cpu) == INT_TIMER, "a busy loop did not come back to the kernel");
    check(cpu.r.rip == CODE, "the busy loop's rip moved");
    // and a poke ends a run at once
    cpu.poked_ptr = &cpu._poked;
    cpu._poked = true;
    check(run(&mem, &cpu) == INT_TIMER, "a poked run did not come back");
    check(!cpu._poked, "the poke was not taken");
    teardown(&mem, &cpu);
}

// mov rax, fs:[0]; syscall
static void test_fs_base(void) {
    static const uint8_t code[] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0, 0x0f, 0x05};
    struct mem mem;
    struct cpu_state cpu;
    setup(&mem, &cpu, code, sizeof(code));
    map(&mem, DATA, 1, P_WRITE);
    uint64_t tls = 0xfeedfacecafebeefull;
    poke(&mem, DATA + 0x10, &tls, 8);
    cpu.r.fs_base = DATA + 0x10;
    check(run(&mem, &cpu) == INT_SYSCALL, "the run did not reach its syscall");
    check(cpu.r.rax == tls, "fs:[0] read %#llx", (unsigned long long) cpu.r.rax);
    teardown(&mem, &cpu);
}

int main(void) {
    test_syscall();
    test_stack_and_flags();
    test_copy_on_write();
    test_cmpxchg16b();
    test_faults();
    test_push();
    test_traps();
    test_fs_base();
    if (failures)
        return 1;
    printf("ok\n");
    return 0;
}
