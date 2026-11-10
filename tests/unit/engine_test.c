// The blink engine on raw x86-64, with no loader and no kernel: each test
// maps memory, writes hand-assembled instructions into it, runs them through
// cpu_run_to_interrupt and checks how the run came back, the registers, and
// the memory. Built only for the x86_64 guest. SSE4, and the SSE2 compares,
// are checked against what an x86-64 CPU gave for the same instructions and
// inputs rather than against answers worked out by hand: see sse4_cases.h.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu/cpu.h"
#include "emu/engine.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"
#include "kernel/memory.h"
#include "sse4_cases.h"
#include "sse4_expected.h"

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

// Read guest memory within one page.
static void peek(struct mem *mem, uint64_t addr, void *out, size_t n) {
    read_wrlock(&mem->lock);
    memcpy(out, mem_ptr(mem, addr, MEM_READ), n);
    read_wrunlock(&mem->lock);
}

static uint64_t peek64(struct mem *mem, uint64_t addr) {
    uint64_t value;
    peek(mem, addr, &value, sizeof(value));
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

    // a misaligned movaps is #GP(0), not a fault on a page the kernel can
    // page in and restart forever: the address is 0, which nothing maps
    static const uint8_t movaps[] = {0x0f, 0x28, 0x07, 0x0f, 0x05};  // movaps xmm0, [rdi]; syscall
    setup(&mem, &cpu, movaps, sizeof(movaps));
    map(&mem, DATA, 1, P_WRITE);
    cpu.r.rdi = DATA + 8;
    check(run(&mem, &cpu) == INT_GPF, "a misaligned movaps did not fault");
    check(cpu.segfault_addr == 0, "the misaligned movaps faulted at %#llx, a page the kernel would restart on",
            (unsigned long long) cpu.segfault_addr);
    check(cpu.r.rip == CODE, "the faulting movaps's rip moved");
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

// Runs code from the registers in, the xmm file included, and returns the
// registers it stopped with. syscall leaves rip in rcx and rflags in r11, so
// a test reads its results elsewhere.
static struct x86_64_regs run_regs(const uint8_t *code, size_t n, struct x86_64_regs in) {
    struct mem mem;
    struct cpu_state cpu;
    setup(&mem, &cpu, code, n);
    memcpy(cpu.r.gpr, in.gpr, sizeof(in.gpr));
    cpu.r.rsp = STACK_TOP;
    memcpy(cpu.r.xmm, in.xmm, sizeof(in.xmm));
    check(run(&mem, &cpu) == INT_SYSCALL, "the run did not reach its syscall");
    struct x86_64_regs r = cpu.r;
    teardown(&mem, &cpu);
    return r;
}

// rdi and rsi as given and the registers a scan writes holding a marker.
static struct x86_64_regs marked(uint64_t rdi, uint64_t rsi) {
    return (struct x86_64_regs) {
        .rax = 0x5a5a5a5a5a5a5a5aull, .rbx = 0x5a5a5a5a5a5a5a5aull,
        .rdx = 0x5a5a5a5a5a5a5a5aull, .rdi = rdi, .rsi = rsi,
    };
}

#define ZF (1 << 6)
#define CF (1 << 0)

// bsr and bsf of zero set ZF and leave the destination as it was: AMD
// documents it, Intel does it, and LLVM leans on it, seeding the
// destination with the answer for zero before the scan. An emulator that
// writes 0 there makes u64::leading_zeros(0) 63.
//   bsr rax, rdi; bsf rbx, rdi; syscall
// lzcnt and tzcnt count, and say a zero source in CF:
//   lzcnt rbx, rsi; tzcnt rdx, rsi; syscall
//   lzcnt ebx, esi; tzcnt edx, esi; syscall
static void test_bitscan(void) {
    static const uint8_t scan[] = {
        0x48, 0x0f, 0xbd, 0xc7,  // bsr rax, rdi
        0x48, 0x0f, 0xbc, 0xdf,  // bsf rbx, rdi
        0x0f, 0x05,
    };
    struct x86_64_regs r = run_regs(scan, sizeof(scan), marked(0, 0));
    check(r.rax == 0x5a5a5a5a5a5a5a5aull, "bsr of zero wrote %#llx", (unsigned long long) r.rax);
    check(r.rbx == 0x5a5a5a5a5a5a5a5aull, "bsf of zero wrote %#llx", (unsigned long long) r.rbx);
    check(r.rflags & ZF, "bsf of zero did not set ZF");
    r = run_regs(scan, sizeof(scan), marked(0x0000100000000010ull, 0));
    check(r.rax == 44 && r.rbx == 4, "bsr %llu bsf %llu of 1<<44|1<<4",
            (unsigned long long) r.rax, (unsigned long long) r.rbx);
    check(!(r.rflags & ZF), "bsf of nonzero set ZF");

    static const uint8_t count64[] = {
        0xf3, 0x48, 0x0f, 0xbd, 0xde,  // lzcnt rbx, rsi
        0xf3, 0x48, 0x0f, 0xbc, 0xd6,  // tzcnt rdx, rsi
        0x0f, 0x05,
    };
    r = run_regs(count64, sizeof(count64), marked(0, 0x0000100000000010ull));
    check(r.rbx == 19 && r.rdx == 4, "lzcnt %llu tzcnt %llu of 1<<44|1<<4",
            (unsigned long long) r.rbx, (unsigned long long) r.rdx);
    check(!(r.rflags & CF), "tzcnt of nonzero set CF");
    r = run_regs(count64, sizeof(count64), marked(0, 0));
    check(r.rbx == 64 && r.rdx == 64, "lzcnt %llu tzcnt %llu of zero",
            (unsigned long long) r.rbx, (unsigned long long) r.rdx);
    check(r.rflags & CF, "tzcnt of zero did not set CF");

    static const uint8_t count32[] = {
        0xf3, 0x0f, 0xbd, 0xde,  // lzcnt ebx, esi
        0xf3, 0x0f, 0xbc, 0xd6,  // tzcnt edx, esi
        0x0f, 0x05,
    };
    r = run_regs(count32, sizeof(count32), marked(0, 0x10));
    check(r.rbx == 27 && r.rdx == 4, "lzcnt %llu tzcnt %llu of 32-bit 1<<4",
            (unsigned long long) r.rbx, (unsigned long long) r.rdx);
}

// Every case in sse4_cases.h, run by blink, against what an x86-64 CPU gave
// for it (sse4_expected.h, which sse4_oracle.sh writes). A case that differs
// prints what blink gave, to set beside what `sse4_oracle.sh <case>` prints.
static void run_case(const struct sse4_case *c, struct sse4_state *s) {
    uint8_t code[sizeof(sse4_prologue) + sizeof(c->bytes) + sizeof(sse4_epilogue) + 2];
    size_t n = 0;
    memcpy(code + n, sse4_prologue, sizeof(sse4_prologue));
    n += sizeof(sse4_prologue);
    memcpy(code + n, c->bytes, c->n);
    n += c->n;
    memcpy(code + n, sse4_epilogue, sizeof(sse4_epilogue));
    n += sizeof(sse4_epilogue);
    code[n++] = 0x0f;  // syscall
    code[n++] = 0x05;
    struct mem mem;
    struct cpu_state cpu;
    setup(&mem, &cpu, code, n);
    map(&mem, DATA, 1, P_WRITE);
    map(&mem, STACK_TOP - 0x1000, 1, P_WRITE);  // for the flags' push and pop
    poke(&mem, DATA, s, sizeof(*s));
    cpu.r.rdi = DATA;
    check(run(&mem, &cpu) == INT_SYSCALL, "%s did not reach its syscall", c->name);
    peek(&mem, DATA, s, sizeof(*s));
    teardown(&mem, &cpu);
}

static void test_against_hardware(void) {
    static struct sse4_case cases[SSE4_MAX_CASES];
    int n = sse4_cases(cases);
    int expected = sizeof(sse4_expected) / sizeof(*sse4_expected);
    check(n == expected, "%d cases and %d results: rerun tests/unit/sse4_oracle.sh", n, expected);
    for (int i = 0; i < n && i < expected; i++) {
        check(strcmp(cases[i].name, sse4_expected[i].name) == 0, "case %d is %s here and %s in the results",
                i, cases[i].name, sse4_expected[i].name);
        struct sse4_state out[SSE4_INPUTS];
        uint64_t hash = SSE4_HASH_SEED;
        for (int j = 0; j < SSE4_INPUTS; j++) {
            out[j] = (struct sse4_state) {0};
            sse4_input(j, &out[j]);
            run_case(&cases[i], &out[j]);
            hash = sse4_hash(hash, &cases[i], &out[j]);
        }
        if (hash != sse4_expected[i].hash) {
            check(false, "%s differs from the hardware's", cases[i].name);
            for (int j = 0; j < SSE4_INPUTS; j++)
                sse4_print("out", j, &cases[i], &out[j]);
        }
    }
}

static void test_vex(void) {
    static const uint8_t refused[][7] = {
        {0xc4, 0xe2, 0x71, 0x3b, 0xc2, 0x0f, 0x05},  // vpminud xmm0, xmm1, xmm2
        {0xc4, 0xe2, 0x71, 0x40, 0xc2, 0x0f, 0x05},  // vpmulld xmm0, xmm1, xmm2
        {0xc5, 0xf1, 0xef, 0xc2, 0x0f, 0x05},        // vpxor xmm0, xmm1, xmm2
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); i++) {
        struct mem mem;
        struct cpu_state cpu;
        setup(&mem, &cpu, refused[i], sizeof(refused[i]));
        check(run(&mem, &cpu) == INT_UNDEFINED, "VEX encoding %zu ran", i);
        teardown(&mem, &cpu);
    }
    static const uint8_t shlx[] = {
        0xc4, 0xe2, 0xc9, 0xf7, 0xdf,  // shlx rbx, rdi, rsi
        0x0f, 0x05,
    };
    struct x86_64_regs r = run_regs(shlx, sizeof(shlx), marked(3, 4));
    check(r.rbx == 48, "shlx gave %llu", (unsigned long long) r.rbx);
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
    test_bitscan();
    test_against_hardware();
    test_vex();
    if (failures)
        return 1;
    printf("ok\n");
    return 0;
}
