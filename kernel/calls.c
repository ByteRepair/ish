#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "emu/interrupt.h"
#include "kernel/memory.h"
#include "kernel/signal.h"
#include "kernel/task.h"

slong_t syscall_stub(void) {
    return _ENOSYS;
}
// While identical, this version of the stub doesn't log below. Use this for
// syscalls that are optional (i.e. fallback on something else) but called
// frequently.
slong_t syscall_silent_stub(void) {
    return _ENOSYS;
}
slong_t syscall_success_stub(void) {
    return 0;
}

#if is_gcc(8) || is_clang(21)
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#if GUEST_X86_64

// x86-64's numbers. Only calls whose arguments, and whatever they read or
// write in the guest's memory, are the same shape on x86-64 as on i386 are
// here so far: a handler that writes an i386 struct (stat, timespec, iovec,
// sigaction, rusage, flock, msghdr) where x86-64 expects its own would be
// worse than ENOSYS. Those arrive with their x86-64 layouts in milestone 5.
syscall_t syscall_table[] = {
    [0]   = (syscall_t) sys_read,
    [1]   = (syscall_t) sys_write,
    [2]   = (syscall_t) sys_open,
    [3]   = (syscall_t) sys_close,
    [7]   = (syscall_t) sys_poll,
    [8]   = (syscall_t) sys_lseek,
    [9]   = (syscall_t) sys_mmap,
    [10]  = (syscall_t) sys_mprotect,
    [11]  = (syscall_t) sys_munmap,
    [12]  = (syscall_t) sys_brk,
    [14]  = (syscall_t) sys_rt_sigprocmask,
    [16]  = (syscall_t) sys_ioctl,
    [17]  = (syscall_t) sys_pread64,
    [18]  = (syscall_t) sys_pwrite64,
    [21]  = (syscall_t) sys_access,
    [22]  = (syscall_t) sys_pipe,
    [24]  = (syscall_t) sys_sched_yield,
    [25]  = (syscall_t) sys_mremap,
    [26]  = (syscall_t) sys_msync,
    [28]  = (syscall_t) sys_madvise,
    [32]  = (syscall_t) sys_dup,
    [33]  = (syscall_t) sys_dup2,
    [34]  = (syscall_t) sys_pause,
    [39]  = (syscall_t) sys_getpid,
    [57]  = (syscall_t) sys_fork,
    [58]  = (syscall_t) sys_vfork,
    [59]  = (syscall_t) sys_execve,
    [60]  = (syscall_t) sys_exit,
    [62]  = (syscall_t) sys_kill,
    [63]  = (syscall_t) sys_uname,
    [73]  = (syscall_t) sys_flock,
    [74]  = (syscall_t) sys_fsync,
    [75]  = (syscall_t) sys_fsync, // fdatasync
    [79]  = (syscall_t) sys_getcwd,
    [80]  = (syscall_t) sys_chdir,
    [81]  = (syscall_t) sys_fchdir,
    [82]  = (syscall_t) sys_rename,
    [83]  = (syscall_t) sys_mkdir,
    [84]  = (syscall_t) sys_rmdir,
    [86]  = (syscall_t) sys_link,
    [87]  = (syscall_t) sys_unlink,
    [88]  = (syscall_t) sys_symlink,
    [89]  = (syscall_t) sys_readlink,
    [90]  = (syscall_t) sys_chmod,
    [91]  = (syscall_t) sys_fchmod,
    [92]  = (syscall_t) sys_chown32,
    [93]  = (syscall_t) sys_fchown32,
    [94]  = (syscall_t) sys_lchown,
    [95]  = (syscall_t) sys_umask,
    [102] = (syscall_t) sys_getuid,
    [103] = (syscall_t) sys_syslog,
    [104] = (syscall_t) sys_getgid,
    [105] = (syscall_t) sys_setuid,
    [106] = (syscall_t) sys_setgid,
    [107] = (syscall_t) sys_geteuid,
    [108] = (syscall_t) sys_getegid,
    [109] = (syscall_t) sys_setpgid,
    [110] = (syscall_t) sys_getppid,
    [111] = (syscall_t) sys_getpgrp,
    [112] = (syscall_t) sys_setsid,
    [113] = (syscall_t) sys_setreuid,
    [114] = (syscall_t) sys_setregid,
    [115] = (syscall_t) sys_getgroups,
    [116] = (syscall_t) sys_setgroups,
    [117] = (syscall_t) sys_setresuid,
    [118] = (syscall_t) sys_getresuid,
    [119] = (syscall_t) sys_setresgid,
    [120] = (syscall_t) sys_getresgid,
    [121] = (syscall_t) sys_getpgid,
    [124] = (syscall_t) sys_getsid,
    [125] = (syscall_t) sys_capget,
    [126] = (syscall_t) sys_capset,
    [127] = (syscall_t) sys_rt_sigpending,
    [133] = (syscall_t) sys_mknod,
    [135] = (syscall_t) sys_personality,
    [140] = (syscall_t) sys_getpriority,
    [141] = (syscall_t) sys_setpriority,
    [149] = (syscall_t) sys_mlock,
    [157] = (syscall_t) sys_prctl,
    [161] = (syscall_t) sys_chroot,
    [162] = (syscall_t) syscall_success_stub, // sync
    [165] = (syscall_t) sys_mount,
    [166] = (syscall_t) sys_umount2,
    [170] = (syscall_t) sys_sethostname,
    [186] = (syscall_t) sys_gettid,
    [200] = (syscall_t) sys_tkill,
    [213] = (syscall_t) sys_epoll_create0,
    [217] = (syscall_t) sys_getdents64,
    [218] = (syscall_t) sys_set_tid_address,
    [231] = (syscall_t) sys_exit_group,
    [232] = (syscall_t) sys_epoll_wait,
    [233] = (syscall_t) sys_epoll_ctl,
    [234] = (syscall_t) sys_tgkill,
    [237] = (syscall_t) sys_mbind,
    [257] = (syscall_t) sys_openat,
    [258] = (syscall_t) sys_mkdirat,
    [259] = (syscall_t) sys_mknodat,
    [260] = (syscall_t) sys_fchownat,
    [263] = (syscall_t) sys_unlinkat,
    [264] = (syscall_t) sys_renameat,
    [265] = (syscall_t) sys_linkat,
    [266] = (syscall_t) sys_symlinkat,
    [267] = (syscall_t) sys_readlinkat,
    [268] = (syscall_t) sys_fchmodat,
    [269] = (syscall_t) sys_faccessat,
    [284] = (syscall_t) sys_eventfd,
    [290] = (syscall_t) sys_eventfd2,
    [291] = (syscall_t) sys_epoll_create,
    [292] = (syscall_t) sys_dup3,
    [293] = (syscall_t) sys_pipe2,
    [316] = (syscall_t) sys_renameat2,
    [318] = (syscall_t) sys_getrandom,
    [324] = (syscall_t) syscall_silent_stub, // membarrier
    [326] = (syscall_t) sys_copy_file_range,
    [332] = (syscall_t) sys_statx,
    [439] = (syscall_t) syscall_silent_stub, // faccessat2
};

#else

syscall_t syscall_table[] = {
    [1]   = (syscall_t) sys_exit,
    [2]   = (syscall_t) sys_fork,
    [3]   = (syscall_t) sys_read,
    [4]   = (syscall_t) sys_write,
    [5]   = (syscall_t) sys_open,
    [6]   = (syscall_t) sys_close,
    [7]   = (syscall_t) sys_waitpid,
    [9]   = (syscall_t) sys_link,
    [10]  = (syscall_t) sys_unlink,
    [11]  = (syscall_t) sys_execve,
    [12]  = (syscall_t) sys_chdir,
    [13]  = (syscall_t) sys_time,
    [14]  = (syscall_t) sys_mknod,
    [15]  = (syscall_t) sys_chmod,
    [19]  = (syscall_t) sys_lseek,
    [20]  = (syscall_t) sys_getpid,
    [21]  = (syscall_t) sys_mount,
    [23]  = (syscall_t) sys_setuid,
    [24]  = (syscall_t) sys_getuid,
    [25]  = (syscall_t) sys_stime,
    [26]  = (syscall_t) sys_ptrace,
    [27]  = (syscall_t) sys_alarm,
    [29]  = (syscall_t) sys_pause,
    [30]  = (syscall_t) sys_utime,
    [33]  = (syscall_t) sys_access,
    [36]  = (syscall_t) syscall_success_stub, // sync
    [37]  = (syscall_t) sys_kill,
    [38]  = (syscall_t) sys_rename,
    [39]  = (syscall_t) sys_mkdir,
    [40]  = (syscall_t) sys_rmdir,
    [41]  = (syscall_t) sys_dup,
    [42]  = (syscall_t) sys_pipe,
    [43]  = (syscall_t) sys_times,
    [45]  = (syscall_t) sys_brk,
    [46]  = (syscall_t) sys_setgid,
    [47]  = (syscall_t) sys_getgid,
    [49]  = (syscall_t) sys_geteuid,
    [50]  = (syscall_t) sys_getegid,
    [52]  = (syscall_t) sys_umount2,
    [54]  = (syscall_t) sys_ioctl,
    [55]  = (syscall_t) sys_fcntl32,
    [57]  = (syscall_t) sys_setpgid,
    [60]  = (syscall_t) sys_umask,
    [61]  = (syscall_t) sys_chroot,
    [63]  = (syscall_t) sys_dup2,
    [64]  = (syscall_t) sys_getppid,
    [65]  = (syscall_t) sys_getpgrp,
    [66]  = (syscall_t) sys_setsid,
    [74]  = (syscall_t) sys_sethostname,
    [75]  = (syscall_t) sys_setrlimit32,
    [76]  = (syscall_t) sys_old_getrlimit32,
    [77]  = (syscall_t) sys_getrusage,
    [78]  = (syscall_t) sys_gettimeofday,
    [79]  = (syscall_t) sys_settimeofday,
    [80]  = (syscall_t) sys_getgroups,
    [81]  = (syscall_t) sys_setgroups,
    [83]  = (syscall_t) sys_symlink,
    [85]  = (syscall_t) sys_readlink,
    [88]  = (syscall_t) sys_reboot,
    [90]  = (syscall_t) sys_mmap,
    [91]  = (syscall_t) sys_munmap,
    [94]  = (syscall_t) sys_fchmod,
    [96]  = (syscall_t) sys_getpriority,
    [97]  = (syscall_t) sys_setpriority,
    [99]  = (syscall_t) sys_statfs,
    [100] = (syscall_t) sys_fstatfs,
    [102] = (syscall_t) sys_socketcall,
    [103] = (syscall_t) sys_syslog,
    [104] = (syscall_t) sys_setitimer,
    [114] = (syscall_t) sys_wait4,
    [116] = (syscall_t) sys_sysinfo,
    [117] = (syscall_t) sys_ipc,
    [118] = (syscall_t) sys_fsync,
    [119] = (syscall_t) sys_sigreturn,
    [120] = (syscall_t) sys_clone,
    [122] = (syscall_t) sys_uname,
    [125] = (syscall_t) sys_mprotect,
    [132] = (syscall_t) sys_getpgid,
    [133] = (syscall_t) sys_fchdir,
    [136] = (syscall_t) sys_personality,
    [140] = (syscall_t) sys__llseek,
    [141] = (syscall_t) sys_getdents,
    [142] = (syscall_t) sys_select,
    [143] = (syscall_t) sys_flock,
    [144] = (syscall_t) sys_msync,
    [145] = (syscall_t) sys_readv,
    [146] = (syscall_t) sys_writev,
    [147] = (syscall_t) sys_getsid,
    [148] = (syscall_t) sys_fsync, // fdatasync
    [150] = (syscall_t) sys_mlock,
    [155] = (syscall_t) sys_sched_getparam,
    [156] = (syscall_t) sys_sched_setscheduler,
    [157] = (syscall_t) sys_sched_getscheduler,
    [158] = (syscall_t) sys_sched_yield,
    [159] = (syscall_t) sys_sched_get_priority_max,
    [162] = (syscall_t) sys_nanosleep,
    [163] = (syscall_t) sys_mremap,
    [168] = (syscall_t) sys_poll,
    [172] = (syscall_t) sys_prctl,
    [173] = (syscall_t) sys_rt_sigreturn,
    [174] = (syscall_t) sys_rt_sigaction,
    [175] = (syscall_t) sys_rt_sigprocmask,
    [176] = (syscall_t) sys_rt_sigpending,
    [177] = (syscall_t) sys_rt_sigtimedwait,
    [179] = (syscall_t) sys_rt_sigsuspend,
    [180] = (syscall_t) sys_pread,
    [181] = (syscall_t) sys_pwrite,
    [183] = (syscall_t) sys_getcwd,
    [184] = (syscall_t) sys_capget,
    [185] = (syscall_t) sys_capset,
    [186] = (syscall_t) sys_sigaltstack,
    [187] = (syscall_t) sys_sendfile,
    [190] = (syscall_t) sys_vfork,
    [191] = (syscall_t) sys_getrlimit32,
    [192] = (syscall_t) sys_mmap2,
    [193] = (syscall_t) sys_truncate64,
    [194] = (syscall_t) sys_ftruncate64,
    [195] = (syscall_t) sys_stat64,
    [196] = (syscall_t) sys_lstat64,
    [197] = (syscall_t) sys_fstat64,
    [198] = (syscall_t) sys_lchown,
    [199] = (syscall_t) sys_getuid32,
    [200] = (syscall_t) sys_getgid32,
    [201] = (syscall_t) sys_geteuid32,
    [202] = (syscall_t) sys_getegid32,
    [203] = (syscall_t) sys_setreuid,
    [204] = (syscall_t) sys_setregid,
    [205] = (syscall_t) sys_getgroups,
    [206] = (syscall_t) sys_setgroups,
    [207] = (syscall_t) sys_fchown32,
    [208] = (syscall_t) sys_setresuid,
    [209] = (syscall_t) sys_getresuid,
    [210] = (syscall_t) sys_setresgid,
    [211] = (syscall_t) sys_getresgid,
    [212] = (syscall_t) sys_chown32,
    [213] = (syscall_t) sys_setuid,
    [214] = (syscall_t) sys_setgid,
    [215] = (syscall_t) syscall_stub, // setfsuid
    [216] = (syscall_t) syscall_stub, // setfsgid
    [219] = (syscall_t) sys_madvise,
    [220] = (syscall_t) sys_getdents64,
    [221] = (syscall_t) sys_fcntl,
    [224] = (syscall_t) sys_gettid,
    [225] = (syscall_t) syscall_success_stub, // readahead
    [226 ... 237] = (syscall_t) sys_xattr_stub,
    [238] = (syscall_t) sys_tkill,
    [239] = (syscall_t) sys_sendfile64,
    [240] = (syscall_t) sys_futex,
    [241] = (syscall_t) sys_sched_setaffinity,
    [242] = (syscall_t) sys_sched_getaffinity,
    [243] = (syscall_t) sys_set_thread_area,
    [245] = (syscall_t) syscall_stub, // io_setup
    [252] = (syscall_t) sys_exit_group,
    [254] = (syscall_t) sys_epoll_create0,
    [255] = (syscall_t) sys_epoll_ctl,
    [256] = (syscall_t) sys_epoll_wait,
    [258] = (syscall_t) sys_set_tid_address,
    [259] = (syscall_t) sys_timer_create,
    [260] = (syscall_t) sys_timer_settime,
    [263] = (syscall_t) sys_timer_delete,
    [264] = (syscall_t) sys_clock_settime,
    [265] = (syscall_t) sys_clock_gettime,
    [266] = (syscall_t) sys_clock_getres,
    [268] = (syscall_t) sys_statfs64,
    [269] = (syscall_t) sys_fstatfs64,
    [270] = (syscall_t) sys_tgkill,
    [271] = (syscall_t) sys_utimes,
    [272] = (syscall_t) syscall_success_stub,
    [274] = (syscall_t) sys_mbind,
    [284] = (syscall_t) sys_waitid,
    [289] = (syscall_t) sys_ioprio_set,
    [290] = (syscall_t) sys_ioprio_get,
    [291] = (syscall_t) syscall_stub, // inotify_init
    [295] = (syscall_t) sys_openat,
    [296] = (syscall_t) sys_mkdirat,
    [297] = (syscall_t) sys_mknodat,
    [298] = (syscall_t) sys_fchownat,
    [300] = (syscall_t) sys_fstatat64,
    [301] = (syscall_t) sys_unlinkat,
    [302] = (syscall_t) sys_renameat,
    [303] = (syscall_t) sys_linkat,
    [304] = (syscall_t) sys_symlinkat,
    [305] = (syscall_t) sys_readlinkat,
    [306] = (syscall_t) sys_fchmodat,
    [307] = (syscall_t) sys_faccessat,
    [308] = (syscall_t) sys_pselect,
    [309] = (syscall_t) sys_ppoll,
    [311] = (syscall_t) sys_set_robust_list,
    [312] = (syscall_t) sys_get_robust_list,
    [313] = (syscall_t) sys_splice,
    [319] = (syscall_t) sys_epoll_pwait,
    [320] = (syscall_t) sys_utimensat,
    [322] = (syscall_t) sys_timerfd_create,
    [323] = (syscall_t) sys_eventfd,
    [324] = (syscall_t) sys_fallocate,
    [325] = (syscall_t) sys_timerfd_settime,
    [328] = (syscall_t) sys_eventfd2,
    [329] = (syscall_t) sys_epoll_create,
    [330] = (syscall_t) sys_dup3,
    [331] = (syscall_t) sys_pipe2,
    [332] = (syscall_t) syscall_stub, // inotify_init1
    [340] = (syscall_t) sys_prlimit64,
    [345] = (syscall_t) sys_sendmmsg,
    [352] = (syscall_t) syscall_stub, // sched_getattr
    [353] = (syscall_t) sys_renameat2,
    [355] = (syscall_t) sys_getrandom,
    [359] = (syscall_t) sys_socket,
    [360] = (syscall_t) sys_socketpair,
    [361] = (syscall_t) sys_bind,
    [362] = (syscall_t) sys_connect,
    [363] = (syscall_t) sys_listen,
    [364] = (syscall_t) syscall_stub, // accept4
    [365] = (syscall_t) sys_getsockopt,
    [366] = (syscall_t) sys_setsockopt,
    [367] = (syscall_t) sys_getsockname,
    [368] = (syscall_t) sys_getpeername,
    [369] = (syscall_t) sys_sendto,
    [370] = (syscall_t) sys_sendmsg,
    [371] = (syscall_t) sys_recvfrom,
    [372] = (syscall_t) sys_recvmsg,
    [373] = (syscall_t) sys_shutdown,
    [375] = (syscall_t) syscall_silent_stub, // membarrier
    [377] = (syscall_t) sys_copy_file_range,
    [383] = (syscall_t) sys_statx,
    [384] = (syscall_t) sys_arch_prctl,
    [422] = (syscall_t) syscall_silent_stub, // futex_time64
    [439] = (syscall_t) syscall_silent_stub, // faccessat2
};

#endif

#define NUM_SYSCALLS (sizeof(syscall_table) / sizeof(syscall_table[0]))

void dump_stack(int lines);

void handle_interrupt(int interrupt) {
    struct cpu_state *cpu = &current->cpu;
    if (interrupt == INT_SYSCALL) {
        ulong_t syscall_num = CPU_SYSCALL_NR(cpu);
        if (syscall_num >= NUM_SYSCALLS || syscall_table[syscall_num] == NULL) {
            printk("%d(%s) missing syscall %d\n", current->pid, current->comm, syscall_num);
            CPU_SYSCALL_RESULT(cpu) = _ENOSYS;
        } else {
            if (syscall_table[syscall_num] == (syscall_t) syscall_stub) {
                printk("%d(%s) stub syscall %d\n", current->pid, current->comm, syscall_num);
            }
            STRACE("%d call %-3d ", current->pid, syscall_num);
            slong_t result = syscall_table[syscall_num](CPU_SYSCALL_ARGS(cpu));
            STRACE(" = 0x%x\n", result);
            CPU_SYSCALL_RESULT(cpu) = result;
        }
    } else if (interrupt == INT_GPF) {
        // some page faults, such as stack growing or CoW clones, are handled by mem_ptr
        read_wrlock(&current->mem->lock);
        void *ptr = mem_ptr(current->mem, cpu->segfault_addr, cpu->segfault_was_write ? MEM_WRITE : MEM_READ);
        read_wrunlock(&current->mem->lock);
        if (ptr == NULL) {
            printk("%d page fault on 0x%x at 0x%x\n", current->pid, cpu->segfault_addr, CPU_IP(cpu));
            struct siginfo_ info = {
                .code = mem_segv_reason(current->mem, cpu->segfault_addr),
                .fault.addr = cpu->segfault_addr,
            };
            dump_stack(8);
            deliver_signal(current, SIGSEGV_, info);
        }
    } else if (interrupt == INT_UNDEFINED) {
        printk("%d illegal instruction at 0x%x: ", current->pid, CPU_IP(cpu));
        for (int i = 0; i < 8; i++) {
            uint8_t b;
            if (user_get(CPU_IP(cpu) + i, b))
                break;
            printk("%02x ", b);
        }
        printk("\n");
        dump_stack(8);
        struct siginfo_ info = {
            .code = SI_KERNEL_,
            .fault.addr = CPU_IP(cpu),
        };
        deliver_signal(current, SIGILL_, info);
    } else if (interrupt == INT_DIV || interrupt == INT_FPU) {
        // asbestos raises neither; blink raises both
        struct siginfo_ info = {
            .code = interrupt == INT_DIV ? FPE_INTDIV_ : FPE_FLTINV_,
            .fault.addr = CPU_IP(cpu),
        };
        deliver_signal(current, SIGFPE_, info);
    } else if (interrupt == INT_BREAKPOINT) {
        lock(&pids_lock);
        send_signal(current, SIGTRAP_, (struct siginfo_) {
            .sig = SIGTRAP_,
            .code = SI_KERNEL_,
        });
        unlock(&pids_lock);
    } else if (interrupt == INT_DEBUG) {
        lock(&pids_lock);
        send_signal(current, SIGTRAP_, (struct siginfo_) {
            .sig = SIGTRAP_,
            .code = TRAP_TRACE_,
        });
        unlock(&pids_lock);
    } else if (interrupt != INT_TIMER) {
        printk("%d unhandled interrupt %d\n", current->pid, interrupt);
        sys_exit(interrupt);
    }

    receive_signals();
    struct tgroup *group = current->group;
    lock(&group->lock);
    while (group->stopped)
        wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
    unlock(&group->lock);
}

void dump_maps(void) {
    extern void proc_maps_dump(struct task *task, struct proc_data *buf);
    struct proc_data buf = {};
    proc_maps_dump(current, &buf);
    // go a line at a time because it can be fucking enormous
    char *orig_data = buf.data;
    while (buf.size > 0) {
        size_t chunk_size = buf.size;
        if (chunk_size > 1024)
            chunk_size = 1024;
        printk("%.*s", chunk_size, buf.data);
        buf.data += chunk_size;
        buf.size -= chunk_size;
    }
    free(orig_data);
}

void dump_mem(addr_t start, uint_t len) {
    const int width = 8;
    for (addr_t addr = start; addr < start + len; addr += sizeof(dword_t)) {
        unsigned from_left = (addr - start) / sizeof(dword_t) % width;
        if (from_left == 0)
            printk("%08x: ", addr);
        dword_t word;
        if (user_get(addr, word))
            break;
        printk("%08x ", word);
        if (from_left == width - 1)
            printk("\n");
    }
}

void dump_stack(int lines) {
    printk("stack at %x, base at %x, ip at %x\n", CPU_SP(&current->cpu), CPU_BP(&current->cpu), CPU_IP(&current->cpu));
    dump_mem(CPU_SP(&current->cpu), lines * sizeof(dword_t) * 8);
}

// TODO find a home for this
#ifdef LOG_OVERRIDE
int log_override = 0;
#endif
