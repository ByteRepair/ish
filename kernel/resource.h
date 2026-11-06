#ifndef RESOURCE_H
#define RESOURCE_H
#include "kernel/time.h"

typedef qword_t rlim_t_;
typedef dword_t rlim32_t_;
#define RLIM_INFINITY_ ((rlim_t_) -1)

struct rlimit_ {
    rlim_t_ cur;
    rlim_t_ max;
};

struct rlimit32_ {
    rlim32_t_ cur;
    rlim32_t_ max;
};

#define RLIMIT_CPU_ 0
#define RLIMIT_FSIZE_ 1
#define RLIMIT_DATA_ 2
#define RLIMIT_STACK_ 3
#define RLIMIT_CORE_ 4
#define RLIMIT_RSS_ 5
#define RLIMIT_NPROC_ 6
#define RLIMIT_NOFILE_ 7
#define RLIMIT_MEMLOCK_ 8
#define RLIMIT_AS_ 9
#define RLIMIT_LOCKS_ 10
#define RLIMIT_SIGPENDING_ 11
#define RLIMIT_MSGQUEUE_ 12
#define RLIMIT_NICE_ 13
#define RLIMIT_RTPRIO_ 14
#define RLIMIT_RTTIME_ 15
#define RLIMIT_NLIMITS_ 16

slong_t sys_getrlimit32(dword_t resource, addr_t rlim_addr);
slong_t sys_setrlimit32(dword_t resource, addr_t rlim_addr);
slong_t sys_prlimit64(pid_t_ pid, dword_t resource, addr_t new_limit_addr, addr_t old_limit_addr);
#if GUEST_X86_64
slong_t sys_getrlimit(dword_t resource, addr_t rlim_addr);
slong_t sys_setrlimit(dword_t resource, addr_t rlim_addr);
#endif
slong_t sys_old_getrlimit32(dword_t resource, addr_t rlim_addr);

rlim_t_ rlimit(int resource);

struct rusage_ {
    struct timeval_ utime;
    struct timeval_ stime;
    ulong_t maxrss;
    ulong_t ixrss;
    ulong_t idrss;
    ulong_t isrss;
    ulong_t minflt;
    ulong_t majflt;
    ulong_t nswap;
    ulong_t inblock;
    ulong_t oublock;
    ulong_t msgsnd;
    ulong_t msgrcv;
    ulong_t nsignals;
    ulong_t nvcsw;
    ulong_t nivcsw;
};

struct rusage_ rusage_get_current(void);
void rusage_add(struct rusage_ *dst, struct rusage_ *src);
#define RUSAGE_SELF_ 0
#define RUSAGE_CHILDREN_ -1
slong_t sys_getrusage(dword_t who, addr_t rusage_addr);

slong_t sys_sched_getaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr);
slong_t sys_sched_setaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr);
slong_t sys_getpriority(int_t which, pid_t_ who);
slong_t sys_setpriority(int_t which, pid_t_ who, int_t prio);

slong_t sys_sched_getparam(pid_t_ pid, addr_t param_addr);
slong_t sys_sched_getscheduler(pid_t_ UNUSED(pid));
slong_t sys_sched_setscheduler(pid_t_ UNUSED(pid), int_t policy, addr_t param_addr);
slong_t sys_sched_get_priority_max(int_t policy);

slong_t sys_ioprio_set(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio));
slong_t sys_ioprio_get(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio));

#endif
