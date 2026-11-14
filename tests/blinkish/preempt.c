// Go's async preemption, without Go. Four threads, each on its own
// sigaltstack, check every general and xmm register and the flags in a loop
// while a fifth sends them SIGURG. Each handler uses 2 KiB of stack, as Go's
// does; in mode 'i' it also rewrites the interrupted context the way Go's
// doSigPreempt does (push the PC, point RIP at a routine that saves and
// restores everything, as asyncPreempt does).
//
// It guards two kernel bugs gh died of: the sigaltstack shared by every
// thread (the process dies outright), and the fxsave area placed below the
// signal frame, where the handler's own stack overwrote the xmm registers
// rt_sigreturn put back (exit 40 within a few signals).
//
// Build with LLVM, no libc:
//   clang --target=x86_64-linux-gnu -nostdlib -static -ffreestanding \
//     -fno-stack-protector -fno-pic -O1 -fuse-ld=lld -o preempt preempt.c
// Run under the harness from a directory holding the binary:
//   build-x86_64/ish -r <dir> /preempt n     (and /preempt i)
// Exit codes: 0 ok, 10 a general register wrong, 30-34 flags wrong, 40 xmm.
typedef unsigned long u64;
typedef long i64;

static i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  i64 r;
  register i64 r10 __asm__("r10") = d;
  register i64 r8 __asm__("r8") = e;
  register i64 r9 __asm__("r9") = f;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return r;
}
#define sys(n, a, b, c) sys6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)

static void put(const char *s) {
  u64 n = 0;
  while (s[n]) n++;
  sys(1, 1, s, n);
}
static void putnum(u64 v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do b[--i] = '0' + v % 10; while (v /= 10);
  put(b + i);
}

struct ksigaction { void *handler; u64 flags; void *restorer; u64 mask; };
struct stack_t { void *sp; int flags; int pad; u64 size; };

extern void restorer(void);
extern void injected(void);
extern i64 spin(u64 iterations);
extern i64 thread_entry(void);

__asm__(
    ".text\n"
    "restorer:\n"
    "  mov $15, %eax\n"
    "  syscall\n"
    // asyncPreempt: save flags and every register, clobber them, restore.
    "injected:\n"
    "  pushfq\n"
    "  push %rax\n push %rbx\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n"
    "  push %rbp\n push %r8\n push %r9\n push %r10\n push %r11\n push %r12\n"
    "  push %r13\n push %r14\n push %r15\n"
    "  sub $16, %rsp\n movups %xmm0, (%rsp)\n"
    "  mov $-1, %rax\n mov %rax, %rbx\n mov %rax, %rcx\n mov %rax, %rdx\n"
    "  mov %rax, %rsi\n mov %rax, %rdi\n mov %rax, %rbp\n mov %rax, %r8\n"
    "  mov %rax, %r9\n mov %rax, %r10\n mov %rax, %r11\n mov %rax, %r12\n"
    "  mov %rax, %r13\n mov %rax, %r14\n mov %rax, %r15\n"
    "  pcmpeqd %xmm0, %xmm0\n"
    "  cmp $0, %rax\n" // flags clobbered: ZF=0, CF=0
    "  movups (%rsp), %xmm0\n add $16, %rsp\n"
    "  pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %r11\n pop %r10\n"
    "  pop %r9\n pop %r8\n pop %rbp\n pop %rdi\n pop %rsi\n pop %rdx\n"
    "  pop %rcx\n pop %rbx\n pop %rax\n"
    "  popfq\n"
    "  ret\n"
    // spin(n): every register but rax (scratch) and r12 (the count)
    // holds a known value, xmm0-15 too; each pass checks them all and
    // leans on flags across instruction boundaries.
    "spin:\n"
    "  push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "  mov %rdi, %r12\n"
    "  movabs $0x01fe01fe01fe01fe, %rbx\n"
    "  movabs $0x02fd02fd02fd02fd, %rbp\n"
    "  movabs $0x03fc03fc03fc03fc, %r13\n"
    "  movabs $0x04fb04fb04fb04fb, %r14\n"
    "  movabs $0x05fa05fa05fa05fa, %r15\n"
    "  movabs $0x06f906f906f906f9, %rsi\n"
    "  movabs $0x07f807f807f807f8, %r8\n"
    "  movabs $0x08f708f708f708f7, %r9\n"
    "  movabs $0x09f609f609f609f6, %r10\n"
    "  movabs $0x0af50af50af50af5, %rcx\n"
    "  movabs $0x0bf40bf40bf40bf4, %rdx\n"
    "  movabs $0x0cf30cf30cf30cf3, %rdi\n"
    "  movabs $0x0df20df20df20df2, %r11\n"
    "  movabs $0x15ea15ea15ea15ea, %rax\n movq %rax, %xmm0\n"
    "  movabs $0x16e916e916e916e9, %rax\n movq %rax, %xmm1\n"
    "  movabs $0x17e817e817e817e8, %rax\n movq %rax, %xmm2\n"
    "  movabs $0x18e718e718e718e7, %rax\n movq %rax, %xmm3\n"
    "  movabs $0x19e619e619e619e6, %rax\n movq %rax, %xmm4\n"
    "  movabs $0x1ae51ae51ae51ae5, %rax\n movq %rax, %xmm5\n"
    "  movabs $0x1be41be41be41be4, %rax\n movq %rax, %xmm6\n"
    "  movabs $0x1ce31ce31ce31ce3, %rax\n movq %rax, %xmm7\n"
    "  movabs $0x1de21de21de21de2, %rax\n movq %rax, %xmm8\n"
    "  movabs $0x1ee11ee11ee11ee1, %rax\n movq %rax, %xmm9\n"
    "  movabs $0x1fe01fe01fe01fe0, %rax\n movq %rax, %xmm10\n"
    "  movabs $0x20df20df20df20df, %rax\n movq %rax, %xmm11\n"
    "  movabs $0x21de21de21de21de, %rax\n movq %rax, %xmm12\n"
    "  movabs $0x22dd22dd22dd22dd, %rax\n movq %rax, %xmm13\n"
    "  movabs $0x23dc23dc23dc23dc, %rax\n movq %rax, %xmm14\n"
    "  movabs $0x24db24db24db24db, %rax\n movq %rax, %xmm15\n"
    "1:\n"
    "  movabs $0x01fe01fe01fe01fe, %rax\n cmp %rax, %rbx\n jne 50f\n"
    "  movabs $0x02fd02fd02fd02fd, %rax\n cmp %rax, %rbp\n jne 50f\n"
    "  movabs $0x03fc03fc03fc03fc, %rax\n cmp %rax, %r13\n jne 50f\n"
    "  movabs $0x04fb04fb04fb04fb, %rax\n cmp %rax, %r14\n jne 50f\n"
    "  movabs $0x05fa05fa05fa05fa, %rax\n cmp %rax, %r15\n jne 50f\n"
    "  movabs $0x06f906f906f906f9, %rax\n cmp %rax, %rsi\n jne 50f\n"
    "  movabs $0x07f807f807f807f8, %rax\n cmp %rax, %r8\n jne 50f\n"
    "  movabs $0x08f708f708f708f7, %rax\n cmp %rax, %r9\n jne 50f\n"
    "  movabs $0x09f609f609f609f6, %rax\n cmp %rax, %r10\n jne 50f\n"
    "  movabs $0x0af50af50af50af5, %rax\n cmp %rax, %rcx\n jne 50f\n"
    "  movabs $0x0bf40bf40bf40bf4, %rax\n cmp %rax, %rdx\n jne 50f\n"
    "  movabs $0x0cf30cf30cf30cf3, %rax\n cmp %rax, %rdi\n jne 50f\n"
    "  movabs $0x0df20df20df20df2, %rax\n cmp %rax, %r11\n jne 50f\n"
    "  movq %xmm0, %rax\n push %rax\n movabs $0x15ea15ea15ea15ea, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm1, %rax\n push %rax\n movabs $0x16e916e916e916e9, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm2, %rax\n push %rax\n movabs $0x17e817e817e817e8, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm3, %rax\n push %rax\n movabs $0x18e718e718e718e7, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm4, %rax\n push %rax\n movabs $0x19e619e619e619e6, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm5, %rax\n push %rax\n movabs $0x1ae51ae51ae51ae5, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm6, %rax\n push %rax\n movabs $0x1be41be41be41be4, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm7, %rax\n push %rax\n movabs $0x1ce31ce31ce31ce3, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm8, %rax\n push %rax\n movabs $0x1de21de21de21de2, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm9, %rax\n push %rax\n movabs $0x1ee11ee11ee11ee1, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm10, %rax\n push %rax\n movabs $0x1fe01fe01fe01fe0, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm11, %rax\n push %rax\n movabs $0x20df20df20df20df, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm12, %rax\n push %rax\n movabs $0x21de21de21de21de, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm13, %rax\n push %rax\n movabs $0x22dd22dd22dd22dd, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm14, %rax\n push %rax\n movabs $0x23dc23dc23dc23dc, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    "  movq %xmm15, %rax\n push %rax\n movabs $0x24db24db24db24db, %rax\n cmp %rax, (%rsp)\n pop %rax\n jne 60f\n"
    // OF and SF across a boundary: INT64_MAX - (-1) overflows (OF=1, SF=1).
    "  movabs $0x7fffffffffffffff, %rax\n cmp $-1, %rax\n nop\n nop\n nop\n jno 71f\n"
    "  movabs $0x7fffffffffffffff, %rax\n cmp $-1, %rax\n nop\n nop\n nop\n jns 72f\n"
    // jle after a signed compare that overflows: SF!=OF decides it.
    "  movabs $0x7fffffffffffffff, %rax\n cmp $-1, %rax\n nop\n nop\n nop\n jle 73f\n"
    "  mov $1, %eax\n bt $0, %eax\n nop\n nop\n nop\n jae 74f\n"
    "  stc\n nop\n nop\n lea 1(%rax), %rax\n nop\n jnc 70f\n"
    "  clc\n nop\n nop\n lea 1(%rax), %rax\n nop\n jc 70f\n"
    "  dec %r12\n jnz 1b\n"
    "  xor %eax, %eax\n jmp 99f\n"
    "50: mov $10, %eax\n jmp 99f\n"
    "60: mov $40, %eax\n jmp 99f\n"
    "70: mov $30, %eax\n jmp 99f\n"
    "71: mov $31, %eax\n jmp 99f\n"
    "72: mov $32, %eax\n jmp 99f\n"
    "73: mov $33, %eax\n jmp 99f\n"
    "74: mov $34, %eax\n jmp 99f\n"
    "99:\n"
    "  pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n"
    "  ret\n");

#define SPINNERS 4 // the main thread plus three clones
static volatile i64 tids[SPINNERS];
static volatile i64 codes[SPINNERS];
static volatile int running;
static volatile int done;
static volatile u64 handled;
static int mode = 'i';

// A handler that uses real stack, as Go's does: 2 KiB below the frame.
__attribute__((noinline)) static void use_stack(void) {
  volatile char buf[2048];
  for (int i = 0; i < 2048; i++) buf[i] = (char)0xa5;
}

static void handler(int sig, void *info, void *uc) {
  (void)sig, (void)info;
  use_stack();
  __atomic_add_fetch(&handled, 1, __ATOMIC_RELAXED);
  __asm__ volatile("pcmpeqd %%xmm0, %%xmm0\n" "pcmpeqd %%xmm1, %%xmm1\n" "pcmpeqd %%xmm2, %%xmm2\n"
                   "pcmpeqd %%xmm3, %%xmm3\n" "pcmpeqd %%xmm4, %%xmm4\n" "pcmpeqd %%xmm5, %%xmm5\n"
                   "pcmpeqd %%xmm6, %%xmm6\n" "pcmpeqd %%xmm7, %%xmm7\n" "pcmpeqd %%xmm8, %%xmm8\n"
                   "pcmpeqd %%xmm9, %%xmm9\n" "pcmpeqd %%xmm10, %%xmm10\n" "pcmpeqd %%xmm11, %%xmm11\n"
                   "pcmpeqd %%xmm12, %%xmm12\n" "pcmpeqd %%xmm13, %%xmm13\n" "pcmpeqd %%xmm14, %%xmm14\n"
                   "pcmpeqd %%xmm15, %%xmm15\n" ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                   "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
  if (mode != 'i') return;
  u64 *gregs = (u64 *)((char *)uc + 40); // uc_flags, uc_link, uc_stack
  u64 *rsp = &gregs[15], *rip = &gregs[16];
  *rsp -= 8;
  *(u64 *)*rsp = *rip;
  *rip = (u64)injected;
}

static char altstacks[SPINNERS + 1][65536] __attribute__((aligned(16)));
static char stacks[SPINNERS + 1][65536] __attribute__((aligned(16)));

static void set_altstack(char *base) {
  struct stack_t ss = {base, 0, 0, 65536};
  sys(131, &ss, 0, 0);
}

static void spinner(int i) {
  tids[i] = sys(186, 0, 0, 0);
  set_altstack(altstacks[i]);
  __atomic_add_fetch(&running, 1, __ATOMIC_SEQ_CST);
  codes[i] = spin(300000);
}

// Thread index SPINNERS is the preempter: Go's sysmon, signalling every M.
i64 thread_main(i64 i) {
  if (i < SPINNERS) {
    spinner((int)i);
    __atomic_add_fetch(&done, 1, __ATOMIC_SEQ_CST);
    sys(60, 0, 0, 0);
  }
  set_altstack(altstacks[i]);
  i64 pid = sys(39, 0, 0, 0);
  while (running < SPINNERS) sys(24, 0, 0, 0);
  for (u64 n = 0; done < SPINNERS; n++) {
    sys(234, pid, tids[n % SPINNERS], 23);
    for (volatile int k = 0; k < 3000; k++) {}
  }
  sys(60, 0, 0, 0);
  return 0;
}

__asm__(".text\n"
        "thread_entry:\n"
        "  pop %rdi\n" // the index spawn left under the entry address
        "  and $-16, %rsp\n"
        "  call thread_main\n"
        "  hlt\n");

static void spawn(i64 i) {
  u64 *child = (u64 *)(stacks[i] + sizeof(stacks[i]));
  *--child = 0;
  *--child = (u64)i;
  *--child = (u64)thread_entry;
  i64 r;
  __asm__ volatile("syscall\n"
                   "test %%rax, %%rax\n"
                   "jnz 1f\n"
                   "ret\n" // child: pops thread_entry off its new stack
                   "1:\n"
                   : "=a"(r)
                   : "a"(56), "D"(0x00000100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000),
                     "S"(child)
                   : "rcx", "r11", "rdx", "r10", "r8", "r9", "memory");
}

void _start_c(u64 *sp) {
  char **argv = (char **)(sp + 1);
  if (sp[0] > 1) mode = argv[1][0];
  struct ksigaction sa = {(void *)handler, 0x04000000 | 0x08000000 | 4 | 0x10000000,
                          (void *)restorer, 0};
  sys6(13, 23, (i64)&sa, 0, 8, 0, 0);
  for (i64 i = 1; i <= SPINNERS; i++) spawn(i);
  spinner(0);
  __atomic_add_fetch(&done, 1, __ATOMIC_SEQ_CST);
  while (done < SPINNERS) sys(24, 0, 0, 0);
  i64 code = 0;
  for (int i = 0; i < SPINNERS; i++)
    if (codes[i]) code = codes[i];
  put("mode ");
  char m[2] = {(char)mode, 0};
  put(m);
  put(" signals ");
  putnum(handled);
  for (int i = 0; i < SPINNERS; i++) { put(" t"); putnum(i); put("="); putnum(codes[i]); }
  put(code ? " FAIL\n" : " ok\n");
  sys(231, code, 0, 0);
}

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "  mov %rsp, %rdi\n"
        "  and $-16, %rsp\n"
        "  call _start_c\n"
        "  hlt\n");
