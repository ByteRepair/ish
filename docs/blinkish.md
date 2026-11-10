# blinkish

iSH's kernel with blink's interpreter as its CPU: an x86-64 Linux userspace
that runs in-process on iOS, beside the i386 iSH it grew out of. Entoli ships
both, as two terminal environments, with blinkish the default.

This branch (`blinkish`) is Entoli's fork of iSH. It is never force-pushed:
Entoli's submodule pins commits on it, and a pinned commit must stay
reachable. Upstream arrives by a merge.

## Why this shape

iOS refuses an app `fork`, `exec` and `posix_spawn`, and writable-executable
memory. Stock blink hands fork, exec, wait, pids, file descriptors and signals
straight to the host — one guest process per host process — so it cannot run
there as it is. iSH already keeps all of that in-process: tasks are threads,
fork copies an address space it models itself, file descriptors and signals
are its own, and the filesystem is fakefs. What iSH lacks is a 64-bit CPU and
a kernel that speaks the x86-64 ABI. Blink has the CPU — a full x86-64
interpreter (SSE but no AVX, x87, BMI2) that runs with its JIT compiled out,
already shown here running the x86_64 Alpine's busybox on Apple silicon. So the split is:

- **iSH keeps**: tasks and threads, fork/clone/exec, the fd table, signals
  and their delivery, futexes, the VFS (fakefs, realfs, procfs, devfs, ttys,
  pipes, sockets), and the memory map — `struct mem`, mmap, CoW on fork.
- **blink provides**: decode and execute, and nothing else. Its loader,
  syscall layer, VFS, signal handling, JIT, page tables and `main` are not
  compiled in.

The alternative — teaching blink's own kernel to fork and exec in-process —
rebuilds what iSH already has, and keeps none of Entoli's existing driver.

## The seam

Every guest thread runs `task_run_current` (`kernel/task.c`):

    read_wrlock(&mem->lock);
    interrupt = cpu_run_to_interrupt(cpu, &tlb);
    read_wrunlock(&mem->lock);
    handle_interrupt(interrupt);

That loop is the whole contract between CPU and kernel. The blink engine
implements `cpu_run_to_interrupt` over a blink `Machine` owned by the task,
and returns iSH's interrupt numbers: `INT_SYSCALL` for `syscall`, `INT_GPF`
with `segfault_addr`/`segfault_was_write` for a fault, `INT_UNDEFINED` for
`#UD`, and the rest as they map. Memory is iSH's: the blink side translates
a guest address by `mmu_translate` through iSH's TLB, so CoW, stack growth
and file mappings stay iSH's code, and blink's own page tables are never
built. A fault leaves the interpreter at the faulting instruction with its
registers intact, exactly as asbestos does.

## One kernel, two guests

The guest is chosen at compile time: meson's `-Dguest=i386` (default, the
current iSH) or `-Dguest=x86_64`, which defines `GUEST_X86_64` and must be
paired with `-Dengine=blink` (meson refuses either without the other). Under
it, `addr_t`, `uint_t`/`int_t` where they model a C
`long`, `pages_t` and `page_t` become 64-bit, and the arch-specific parts
switch on the macro: the syscall table and its argument registers, ABI
structs, the ELF loader, the signal frame, TLS, the vdso. Everything else
compiles once per guest, unchanged.

A runtime switch (one kernel speaking both ABIs, Linux's compat layer) was
the alternative, and is far larger for no gain: an Entoli process picks one
guest per session, never per syscall.

Entoli links both builds into one app. `tool/ios/ish/build.sh` builds the
library twice and partially links the x86-64 one (`ld -r`) with an
exported-symbols list naming only its `blinkish_*` bridge entry points, so
the two kernels' thousand identical internal names never meet at the final
link; the i386 one keeps its `ish_*` names and its internals global.

## Milestones

Each lands as its own commits on this branch, and each ends in something
runnable on the macOS harness (below) before the next begins.

0. **Groundwork** — done. The fork, blink at `deps/blink`, the read-only
   mounts commit, a macOS harness, the test guests, and stock blink as the
   oracle.
0½. **Bugs found on the way** — done. Each its own commit, fixed in the i386 build
   before anything moves (and offerable upstream): mmap's non-fixed hint
   that is not a hole maps over what is there (`kernel/mmap.c:59-61`);
   mremap dereferences the NULL it tests for (`mmap.c:163`); exec stores
   argc with a host `size_t` write then steps 4 bytes (`kernel/exec.c:359`);
   fcntl's SETLK64 tests the wrong wait constants (`fs/fd.c:369,375`); a
   forked task inherits the parent's `poked_ptr` (`kernel/task.c:59`);
   pread/pwrite take an `off_t_` through `dword_t` slots (`kernel/fs.c:417,
   454`). Milestone 1's type work turned up two more: lseek took its offset
   unsigned, so a seek backwards went to just under 4 GiB, and poll sized a
   host stack array by the guest's nfds, which a large one crashed. Each
   fix carries a guest under `tests/regress/` that fails without it.
1. **The build switch and 64-bit types** — done. `-Dguest`/`-Dengine=blink` in
   meson. Not a global widening: `uint_t`, `int_t` and `dword_t` each stand
   for a C `int` in some places and a `long`, `size_t` or pointer in
   others, and socklen, flags, fds, futex words, ioctl ints and pids stay
   32-bit on x86-64. So a guest `long` pair (`ulong_t`/`slong_t`, 32-bit on
   i386, 64-bit on x86_64) joins `addr_t`, and every prototype in
   `kernel/calls.h` (about 200) and every guest struct is read and given the
   right one; `time_t_` and `clock_t_` become guest longs. `syscall_t`
   takes six guest longs and returns one, so mmap and brk can answer above
   4 GiB and an error sign-extends into rax (`calls.h:258`,
   `calls.c:271-273`, and the socketcall table at `fs/sock.c:1201`, which
   reuses the type). Parameters were retyped by position against the
   x86-64 Linux signatures — sizes, lengths, offsets and `unsigned long`
   arguments — which also caught `getsockopt` taking its length pointer
   as a `dword_t`. The i386 build's code is unchanged by all of it, checked
   by disassembling every kernel object before and after: the only
   differences are the two fixes above and two assert line numbers. The
   x86_64 kernel library compiles; what `-Wshorten-64-to-32` still flags
   there (22 sites) is all in the i386 register file and frames that
   milestones 3, 4 and 6 replace — `cpu->eax` taking the syscall result,
   the auxv entries and initial registers in exec, `cpu.esp` in fork, and
   the signal frames. The fs layer below the syscall boundary
   — fs ops, realfs, fakefs, tmpfs, ttys, poll, locks — and `util/`,
   `platform/` and `main.c` need nothing.
2. **The address space** — done. `struct mem` becomes a four-level table over the
   47-bit user half; `mem_next_page` skips empty subtrees at every level;
   `pt_find_hole` walks from the top skipping full and empty subtrees rather
   than scanning pages (a linear scan of 2^35 pages is not an option); the TLB
   keys on 64-bit pages, and `BAD_PAGE` (0x10000, a real page number) becomes
   a sentinel no page can be. Every loop to `MEM_PAGES` becomes a walk of
   what is mapped: `mem_destroy`, `mm_copy`'s CoW of the whole space,
   `/proc/pid/maps` (which also prints `%08x`), `mem_coredump`, and
   `mem_ptr`'s search for the grows-down mapping above a stack fault, which
   scans page by page today (`kernel/memory.c:244-250`). mmap's layout
   becomes x86-64's: `mmap_base` below the stack, the stack top at
   0x7ffffffff000, the PIE bias 0x555555554000 rather than 0x56555000
   (`exec.c:226`). And `struct mmu` stops holding asbestos: the five places
   `kernel/memory.c` calls it (create, free, invalidate on write) become
   engine hooks, so each engine is told of a written or remapped page.

   As built: one radix table for both guests, two levels of 10 bits for
   i386 and four of 9 for x86-64, each leaf counting its mapped entries.
   Walks (`mem_next_mapped`, and a backward one for mapped or free pages)
   step over a missing directory slot, an empty leaf or a full one whole.
   The hole finder alternates the two backward walks and answers exactly
   what the old page-by-page scan did — the highest hole that fits, at
   its top — which `tests/unit/memory_test.c` checks against a literal
   copy of the scan over forty fixed layouts, for both guests; on i386 a
   process's whole `/proc/self/maps` is byte-for-byte what it was. The
   engine hooks are `emu/engine.h`; the TLB is built for both engines, its
   index masked so a 64-bit address stays in the table. `BAD_PAGE` is all
   ones. `/proc/pid/maps` also stopped running a region on across an
   unmapped gap a whole directory wide.
3. **The engine** — done. `emu/blink/`: the interpreter-only subset of blink's
   sources (the decoder, `machine.c`'s dispatch, the ALU, string, stack,
   x87, SSE and BMI2 ops, `bus.c` for LOCK), built with `-DDISABLE_JIT
   -DNOLINEAR -DNDEBUG -DDISABLE_METAL` and blink's own `config.h` ahead of
   libarchive's on the include path. Dropped: `syscall.c`, `signal.c`, the
   VFS and its fds, the loader, `blink.c`, `demangle.c` (it forks c++filt).
   The glue replaces five things:
   - **`LookupAddress2`** (`memory.c`) becomes iSH's translate: guest
     address and intent in, host pointer or a fault out, through iSH's TLB
     and `mem_ptr`. Every interpreter access reaches it once linear memory
     is off, so blink's page tables are never built — and neither is
     `g_hostpages`, blink's unlocked host-page array.
   - **`HaltMachine`** and the rest of `throw.c` record the trap and fault
     address, rewind the ip where upstream does, clear `m->stashaddr` (a
     fault between a cross-page stash and its commit otherwise writes stale
     data on the next instruction) and `siglongjmp` back to
     `cpu_run_to_interrupt` — never `DeliverSignalToUser`, which can end the
     host process.
   - **`OpSyscall`** longjmps out with `INT_SYSCALL`; iSH's kernel answers
     in rax. RCX and R11 are set as real SYSCALL sets them.
   - **The write intent.** `ReserveAddress` asks for write permission, but
     pushes, `BeginStore`, string and moffs stores, vector stores and the
     cmpxchg8b/16b pointers resolve with read intent and write through it.
     Each of those call sites passes write intent, or a CoW page is written
     in place under another process.
   - **Stubs** for what the dropped files provided: `CheckForSignals`,
     `TerminateSignal`, `UnXlatSignal`, `UnXlatSiCode`,
     `UnlockRobustFutexes`, `InitFds`, `DestroyFds`.

   Faults in blink are not restartable: a push moves RSP before touching
   memory. So every fault a guest is meant to survive — CoW, stack growth,
   a lazily backed page — is resolved inside the translate, which is where
   iSH's `mem_ptr` already does it; only a real SIGSEGV leaves the
   interpreter. Signals arrive between instructions: the kernel delivers
   them only when the CPU comes back, and for a guest that never makes a
   syscall that is asbestos's periodic `INT_TIMER` (`calls.c:318`), so the
   blink engine returns `INT_TIMER` every N instructions as well. `cpu_state` for this guest is the blink `Machine`
   — register file, flags through `ExportFlags`/`ImportFlags`, `fs.base`
   for TLS — with the kernel reaching it through accessors rather than
   fields.

   Blink's own changes (the translate hook, the write intents) are commits
   on the `blinkish` branch of `ByteRepair/blink`, which `deps/blink` follows, under the same
   never-force-push rule as this branch; everything else is iSH-side glue.

   As built: the engine is two files that cannot share headers, joined by
   `emu/blink/glue.h` in plain types. `interp.c` is blink's side — the
   translate hook, the halt path, `OpSyscall`, and stubs for what blink's
   own run loop and page-table walk reach, which abort if ever reached;
   `engine.c` is iSH's — `cpu_run_to_interrupt` and the memory hooks. The
   kernel holds `struct x86_64_regs` (`emu/x86_64.h`, nothing but
   `stdint.h`) and the engine copies it into a per-thread `Machine` for
   each run and back. The kernel reaches registers through `CPU_IP`,
   `CPU_SP`, `CPU_SYSCALL_NR`, `CPU_SYSCALL_ARGS` and the like, which
   expand to i386's field names unchanged, so the i386 build's code is
   unchanged but for a SIGFPE branch blink needs and asbestos never takes.
   The translate goes through iSH's TLB and on a miss through `mem_ptr`,
   and flushes the TLB when that changed a mapping — after a CoW copy, a
   cached read entry would otherwise still point at the shared page. A run
   ends after 16384 instructions or a poke, as `INT_TIMER`.

   `tests/unit/engine_test.c` runs hand-assembled x86-64 through the engine
   with no loader: the syscall stop and its registers, flags, a store
   through each patched path on its own copy-on-write page (each fails with
   blink's write-intent commit taken out), the read after a CoW write,
   faults that stop at their instruction and run again once fixed, a
   faulting push leaving RSP alone, stack growth, `#UD`, `#DE`, the budget,
   a poke, and `fs:`. SSE4, `crc32`, `popcnt` and SSE2's compares it checks
   against x86 hardware instead of answers worked out by hand:
   `tests/unit/sse4_cases.h` lists each instruction and a fixed set of
   inputs, `tests/unit/sse4_oracle.sh` runs them on an x86-64 CPU (Rosetta
   2 on Apple silicon) into `sse4_expected.h`, and the test runs the same
   bytes under blink and compares. Rerun the oracle after changing the
   cases; given a case's name it prints the CPU's inputs and outputs, to
   set beside what the test prints for a case that differs. Rosetta 2
   leaves `ptest`'s AF, OF, PF and SF alone where the SDM clears them, so
   only its ZF and CF are compared. blink's configuration (`emu/blink/config/config.h`)
   carries the five host probes the interpreter reads: without
   `HAVE_MAP_ANONYMOUS`, blink's bus allocation asks mmap for a flag the
   host does not know.

   Signal frames, ptrace and `set_thread_area` are i386 code under
   `#if !GUEST_X86_64`. On x86-64 a handler being called stops ish with a
   message naming milestone 6, ptrace answers ENOSYS until something needs
   it, and the syscall table is still i386's until milestone 4.
4. **exec and a first guest** — done. ELF64 in `kernel/exec.c` and `elf.h`
   (ELFCLASS64, EM_X86_64; ELF64 moves `p_flags` and reorders `Elf_Sym`):
   program headers, `PT_INTERP`, an 8-byte argv/envp stack, 16-byte auxv
   entries without i386's `AT_SYSINFO`, `AT_PLATFORM` "x86_64", and the
   x86-64 initial registers — r8 to r15 zeroed, mxcsr 0x1f80. The x86-64 syscall table (`syscall`, args in rdi, rsi, rdx, r10,
   r8, r9, number in rax), starting with write and exit_group. **Done when**
   `tests/blinkish/hello.S` prints under `ish`.

   As built: each guest has its own ELF layouts under the same field names
   (sizes asserted against the spec), and exec reads whichever it was
   built for. The stack's argc, argv, envp and auxv are guest words. No
   vdso or vvar on x86-64 yet, and no `AT_SYSINFO`. `uname -m` says
   x86_64. The x86-64 table holds every call whose arguments, and whatever
   it reads or writes in guest memory, are already the same shape as on
   i386 — file I/O by descriptor, paths, ids, process control, `pollfd`,
   `epoll_event`, `linux_dirent64`, termios, `statx` — plus `mmap` with
   six arguments and a byte offset, and `pread64`/`pwrite64` with a whole
   offset. What reads or writes a struct whose layout differs waits for
   milestone 5, as ENOSYS rather than an i386 layout in an x86-64 program.

   `tests/regress/` builds for either guest (`run.sh <ish> <root> x86_64`),
   `sys.h` carrying an x86-64 shim and passing 64-bit offsets each
   architecture's way. `exec_stack` checks what exec leaves at the first
   instruction on both: alignment, argc, argv, envp, and the auxv values a
   program depends on. On x86-64, five of seven pass; `pread_high` and
   `fcntl_setlkw` want `ftruncate`, `fcntl` and `wait4`, milestone 5's. The
   i386 build's code is unchanged.
5. **Enough ABI for musl** — done. `arch_prctl` (FS base for TLS; today a stub
   at `kernel/misc.c:27`), and the x86-64 layouts of what differs: `stat`
   (144 bytes, its own field order — `newfstatat`, no stat64), `statfs`
   (all longs, no statfs64), `linux_dirent`, `flock` (unpacked, 32 bytes),
   `iovec`, `msghdr`/`cmsghdr` (size_t lengths, 8-byte alignment),
   `sigaction` (unpacked, long flags), `stack_t`, `siginfo` (128 bytes,
   8-aligned), the timespec family and `rusage`, `sysinfo`,
   `robust_list_head`. `epoll_event`, `statx`, `pollfd`, `ucred`, termios
   and `rlimit` are already right. mmap takes six arguments and a byte
   offset; pread/pwrite/ftruncate/fallocate/lseek take whole 64-bit
   offsets; `SO_RCVTIMEO`/`SO_SNDTIMEO` are 20 and 21, not i386's 66 and
   67. Left in the i386 build alone: socketcall and ipc, mmap2 and
   old_mmap, the `*32` uid calls, `fcntl64`, `_llseek`, `waitpid`,
   rlimit32, `set_thread_area`, sigreturn. `uname -m` x86_64. **Done when** the
   Alpine x86_64 busybox runs through `ld-musl-x86_64.so.1`.

   As built: most of those structs differ only because their fields are C
   longs, so they are spelled in guest longs and are right for both
   guests at once — timespec, timeval and what is made of them, rusage,
   sysinfo, iovec, msghdr and cmsghdr (control messages aligned to a
   long), sigaction's flags, stack_t, linux_dirent, statfs,
   robust_list_head, pselect6's sigmask pair. `stat` has x86-64's own
   struct and converter (`guest_stat_t`), and `flock` is packed on i386
   only, x86-64's `fcntl` mapping its lock commands onto i386's `*LK64`.
   x86-64 alone has six-argument `mmap`, whole-offset `truncate`,
   `ftruncate` and `fallocate`, `getrlimit` and `setrlimit` over the 64-bit
   struct, `arch_prctl` setting and reading the FS and GS bases, and
   `SO_RCVTIMEO`/`SO_SNDTIMEO` as 20 and 21. The x86-64 table maps every
   call i386 has a handler for, at x86-64's numbers, but `clone`.

   Busybox through ld-musl runs `ls`, `stat`, `cat`, `wc`, `date`,
   `md5sum`, `df`, `sort` and builtin-only `sh -c`. `tests/regress/`
   passes on both guests, nine tests now: `abi_structs` reads back
   `stat`, `fstat`, `fstatat` with `AT_EMPTY_PATH`, `timespec`, `iovec`,
   `rusage` and `statfs` against Linux's own definitions for each
   architecture, and fails on x86-64 if `stat` is written i386's way.

   Along the way, and in the i386 build too: `setrlimit` read a 64-bit
   struct where i386 passes two 32-bit words (its own commit, with
   `rlimit`); `stat` zeroes the struct first, so `stat64`'s padding no
   longer carries host stack bytes; `fstatat` honours `AT_EMPTY_PATH`; and
   `sched_getaffinity` answers in whole longs, as Linux does. Those three
   are the only i386 code this milestone changes. `free` shows zeros on
   both guests: the macOS platform layer never fills in memory figures.
6. **clone, fork and signals** — done. The register file is copied on fork and
   clone, and the child re-establishes its own `Machine` on first run
   (`engine_cpu_copied` clears it) — never a deep copy, which would carry
   blink's caches and host pointers. x86-64's clone takes
   `(flags, stack, ptid, ctid, tls)`, not i386's order, so a
   `GUEST_X86_64`-guarded `sys_clone64` reorders the child-tid and TLS
   arguments before the shared `sys_clone`; `CLONE_SETTLS` sets the FS base
   rather than a `user_desc`. The x86-64 `rt_sigframe`
   (`{pretcode, ucontext, siginfo}`, the fxsave area 16-byte aligned below
   the frame, rdi/rsi/rdx = sig, info, uc), returning through the libc's
   `SA_RESTORER` rather than a vdso trampoline — `rt_sigaction` now honours
   that field — and `rt_sigreturn` reads the frame back. The FP and SSE
   state is saved and restored around a handler: i386 iSH never did
   (`signal.c:173`, "TODO"), which mostly went unnoticed there, but on
   x86-64 every float is in an xmm register. **Done when** `busybox sh -c`
   runs pipelines and `trap` works.
7. **A whole Alpine** — done. No vdso at first (musl falls back to syscalls
   when `AT_SYSINFO_EHDR` is absent), an x86-64 vdso later if time calls show
   up in profiles. `apk add git python3` installs all 27 packages and both
   run: `python3 -c 'print(6*7)'` answers 42, and git inits, adds and commits
   (its "dubious ownership" refusal is git reacting to the host-owned realfs
   tree, not an emulator gap — the harness mounts the rootfs as the host
   user). No syscalls had to be added: the x86-64 table already covered what
   apk and python3 exercise, and clone (milestone 6) was the only gate.
8. **Into Entoli.** `build.sh` builds both cores into one xcframework; the
   bridge gains its `blinkish_` twin; Entoli adds an x86_64 rootfs entry for
   iOS, a shell over the second bridge, and the Terminal setting with
   blinkish the default; blink's ISC licence on the licences page.
9. **On a device.** The simulator first, then hardware: correctness, then
   speed against i386 iSH on the same commands. Codex's app-server, the
   first heavily threaded Rust program through it, found four gaps busybox
   and a whole Alpine had not. blink wrote 0 to the destination of a `bsf`
   or `bsr` of zero, where the hardware leaves it alone and LLVM seeds it
   with the answer for zero, so `u64::leading_zeros(0)` came out 63 and
   sharded-slab looked tracing's spans up on the wrong page ("Span not
   found, this is a bug"); its `lzcnt` gave the top bit's index. A
   misaligned `movaps` faulted at its own address, which the kernel paged
   in and restarted forever rather than raising SIGSEGV. `FUTEX_WAIT_BITSET`,
   which every Rust std lock and condvar waits through, answered `ENOSYS`,
   so each blocked thread spun. And `clock_nanosleep`, Rust's
   `thread::sleep`, was missing. Stock blink shares the first two, so it is
   no oracle for them.

   A Bun binary built for x86-64-v2 found the next layer.
   It uses SSE4.1 and SSE4.2 unasked, which blink neither had nor
   advertised (`deps/blink/blink/sse4.c` now runs them). blink wrote a true
   `cmpsd` lane as -1.0 rather than all ones, so JavaScriptCore found
   `Infinity !== Infinity`. `madvise` did nothing, and JavaScriptCore's
   allocator hands decommitted memory out again as zeroed: a hash table got
   its last owner's pointers and a zero count, and probed forever. A
   change to any mapping reset every thread's decoded instructions, which
   the allocators' constant remapping turned into decoding all the time.
   And `sysinfo` on macOS gave no `mem_unit`, so the guest had no memory:
   JavaScriptCore sized its heap from that and collected without end.

   Checking blink against the hardware found three more that nothing had
   hit yet: `popcnt` left AF as it was, `cmpnlt` and `cmpnle` were false of
   a NaN, and `dpps` made the Arm host's positive NaN of infinity times
   zero rather than x86's negative one. It also showed blink running a VEX
   encoding as the legacy instruction of its opcode; with no AVX
   advertised, the decoder now refuses every VEX encoding but BMI2's.

## The harness

Nothing in a milestone needs iOS until 8. On macOS:

    meson setup build-x86_64 -Dguest=x86_64 -Dengine=blink
    ninja -C build-x86_64
    build-x86_64/ish -r <x86_64 rootfs> /bin/busybox sh

with LLVM's clang first on `PATH` for the vdso. The test guests live under
`tests/blinkish/`. Stock blink built on the same host (`./configure
--disable-jit`, `gmake`, run with `-m` — Apple silicon's 16 KiB pages rule out
its linear memory) is the oracle: a guest that behaves differently under
blinkish than under blink is a bug in the port until shown otherwise.

## Risks

- **Speed.** asbestos threads gadgets; blink without its JIT is a decoder and
  a switch. The gap is unmeasured, and it is the number milestone 9 exists to
  produce. Blink's own per-page instruction caching is worth keeping if the
  seam allows it.
- **Blink's globals.** One `Machine` per guest thread is blink's own
  threading model, so many at once is supported; what stays global is the
  `g_bus` LOCK stripes (shared, which keeps LOCK atomic over shared
  memory), the read-only `FLAG_*`, and the thread-local `g_machine`, set
  once per task thread. An assertion in blink ends the process, which is
  why the build is `NDEBUG`.
- **ptrace.** `user_regs_struct`, PEEKUSER's `struct user` and 8-byte
  PEEK/POKE words are x86-64's to write; nothing in the milestones needs
  them, so ptrace stays i386-only until something does (gdb, strace).
- **x87 precision.** Blink keeps x87 registers as doubles, not 80-bit
  extended, so `long double` arithmetic loses precision against real
  hardware. Most of Alpine never notices; numerics that test for it will.
  iSH's `float80` is the fix if one is needed.
- **Self-modifying code.** Blink's instruction cache checks the opcode
  bytes on every hit, so a rewritten instruction is never run stale; what
  needs invalidating on a remap is its cached code page
  (`ResetInstructionCache`), from iSH's TLB flush.
- **The upstream merge.** Every file a milestone touches is a file an
  upstream change can conflict with. Arch-specific code goes in new files
  where it can, and the shared files change by `#if GUEST_X86_64` blocks
  rather than rewrites.
