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
1. **The build switch and 64-bit types.** `-Dguest`/`-Dengine=blink` in
   meson; `misc.h` types per guest; the kernel compiles clean for x86_64 with
   `-Wshorten-64-to-32` on the touched files. The i386 build's behaviour is
   untouched, checked by its own guest still running.
2. **The address space.** `struct mem` becomes a four-level table over the
   47-bit user half; `mem_next_page` skips empty subtrees at every level;
   `pt_find_hole` walks from the top skipping full and empty subtrees rather
   than scanning pages (a linear scan of 2^35 pages is not an option); the TLB
   keys on 64-bit pages. mmap's layout constants become x86-64's (`mmap_base`
   below the stack, `TASK_SIZE` 0x7ffffffff000).
3. **The engine.** `emu/blink/`: the interpreter-only subset of blink's
   sources, built with no JIT, no fork, no metal; a `Machine` per task; memory
   accesses through the translate hook; `syscall` returning `INT_SYSCALL`.
   `cpu_state` gains the x86-64 register file for the guest, or wraps the
   `Machine`.
4. **exec and a first guest.** ELF64 in `kernel/exec.c`: program headers,
   `PT_INTERP`, an 8-byte argv/envp/auxv stack, the x86-64 initial
   registers. The x86-64 syscall table (`syscall`, args in rdi, rsi, rdx, r10,
   r8, r9, number in rax), starting with write and exit_group. **Done when**
   `tests/blinkish/hello.S` prints under `ish`.
5. **Enough ABI for musl.** `arch_prctl` (FS base for TLS), the 64-bit
   layouts of stat, timespec, iovec, sigaction, rlimit, statfs, sysinfo,
   msghdr, epoll_event, dirent64; mmap with a page offset rather than
   mmap2's; pread/pwrite/ftruncate/lseek without split 64-bit pairs; no
   socketcall or ipc multiplexers; `uname -m` x86_64. **Done when** the
   Alpine x86_64 busybox runs through `ld-musl-x86_64.so.1`.
6. **clone, fork and signals.** Copy the `Machine` on fork and clone; the
   x86-64 `rt_sigframe` with its `ucontext` and fxsave-format `fpstate`,
   `SA_RESTORER`, `rt_sigreturn`. **Done when** `busybox sh -c` runs
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
- **Blink's globals.** The interpreter must be safe with one `Machine` per
  guest thread, many at once, in one process; anything it keeps globally
  (flags, the bus lock, signal state) is either made per-machine or held to
  the read-only.
- **Self-modifying code.** iSH invalidates its gadget cache when a page is
  written; blink's decode caching needs the same notification from the
  memory layer.
- **The upstream merge.** Every file a milestone touches is a file an
  upstream change can conflict with. Arch-specific code goes in new files
  where it can, and the shared files change by `#if GUEST_X86_64` blocks
  rather than rewrites.
