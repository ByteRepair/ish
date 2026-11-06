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
current iSH) or `-Dguest=x86_64`, which defines `GUEST_X86_64` and selects
`-Dengine=blink`. Under it, `addr_t`, `uint_t`/`int_t` where they model a C
`long`, `pages_t` and `page_t` become 64-bit, and the arch-specific parts
switch on the macro: the syscall table and its argument registers, ABI
structs, the ELF loader, the signal frame, TLS, the vdso. Everything else
compiles once per guest, unchanged.

A runtime switch (one kernel speaking both ABIs, Linux's compat layer) was
the alternative, and is far larger for no gain: an Entoli process picks one
guest per session, never per syscall.

Entoli links both builds into one app. `tool/ios/ish/build.sh` builds the
library twice and partially links each (`ld -r`) with an exported-symbols
list naming only its bridge entry points — `ish_*` and `blinkish_*` — so the
two kernels' thousand identical internal names never meet at the final link.

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
2. **The address space.** `struct mem` becomes a four-level table over the
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
3. **The engine.** `emu/blink/`: the interpreter-only subset of blink's
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
   on a `ByteRepair/blink` fork the submodule then follows, under the same
   never-force-push rule as this branch; everything else is iSH-side glue.
4. **exec and a first guest.** ELF64 in `kernel/exec.c` and `elf.h`
   (ELFCLASS64, EM_X86_64; ELF64 moves `p_flags` and reorders `Elf_Sym`):
   program headers, `PT_INTERP`, an 8-byte argv/envp stack, 16-byte auxv
   entries without i386's `AT_SYSINFO`, `AT_PLATFORM` "x86_64", and the
   x86-64 initial registers — r8 to r15 zeroed, mxcsr 0x1f80. The x86-64 syscall table (`syscall`, args in rdi, rsi, rdx, r10,
   r8, r9, number in rax), starting with write and exit_group. **Done when**
   `tests/blinkish/hello.S` prints under `ish`.
5. **Enough ABI for musl.** `arch_prctl` (FS base for TLS; today a stub
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
6. **clone, fork and signals.** Copy the `Machine` on fork and clone —
   x86-64's clone takes `(flags, stack, ptid, ctid, tls)`, not i386's
   order, and `CLONE_SETTLS` sets the FS base rather than a `user_desc`.
   The x86-64 `rt_sigframe` (`{pretcode, ucontext, siginfo}`, the fxsave
   area 64-byte aligned, a 128-byte red zone skipped, rdi/rsi/rdx = sig,
   info, uc), returning through the libc's `SA_RESTORER` rather than a
   vdso trampoline, and `rt_sigreturn`. The FP and SSE state is saved and
   restored around a handler: i386 iSH never did (`signal.c:173`, "TODO"),
   which mostly went unnoticed there, but on x86-64 every float is in an
   xmm register. **Done when** `busybox sh -c` runs
   pipelines and `trap` works.
7. **A whole Alpine.** No vdso at first (musl falls back to syscalls when
   `AT_SYSINFO_EHDR` is absent), an x86-64 vdso later if time calls show up in
   profiles. **Done when** `apk add git python3` succeeds and both run, on
   the macOS harness against realfs and fakefs alike.
8. **Into Entoli.** `build.sh` builds both cores into one xcframework; the
   bridge gains its `blinkish_` twin; Entoli adds an x86_64 rootfs entry for
   iOS, a shell over the second bridge, and the Terminal setting with
   blinkish the default; blink's ISC licence on the licences page.
9. **On a device.** The simulator first, then hardware: correctness, then
   speed against i386 iSH on the same commands.

## The harness

Nothing in a milestone needs iOS until 8. On macOS:

    meson setup build-x86_64 -Dguest=x86_64
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
