#ifndef BLINK_CONFIG_H_
#define BLINK_CONFIG_H_

// blink's configuration for iSH: what ./configure would write, chosen by
// hand since only the interpreter is built. blink's own kernel side — its
// syscalls, fds, VFS, loader, signals — is not compiled at all, so most of
// configure's HAVE_ host probes are about code that is not here. The few
// below are read by the interpreter's files, and are what configure finds on
// macOS, which iOS shares: without HAVE_MAP_ANONYMOUS blink substitutes a
// flag value of its own that the host's mmap does not know.

#define DISABLE_JIT          // iOS has no writable-executable memory
#define DISABLE_METAL        // ring 0, real and legacy modes: a user guest has none
#define DISABLE_VFS
#define DISABLE_OVERLAYS
#define DISABLE_STRACE
#define DISABLE_BACKTRACE    // would install SIGSEGV handlers of its own
#define DISABLE_DISASSEMBLER
#define DISABLE_ROM          // no BIOS
#define BLINK_EXTERNAL_MMU   // LookupAddress2 is interp.c's, over iSH's page tables

#define HAVE_MAP_ANONYMOUS
#define HAVE_SCHED_H
#define HAVE_SCHED_YIELD
#define HAVE_SYS_GETENTROPY  // rdrand and rdseed's entropy
#define HAVE_DEV_URANDOM

#endif
