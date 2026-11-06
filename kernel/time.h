#ifndef TIME_H
#define TIME_H
#include "misc.h"

slong_t sys_time(addr_t time_out);
slong_t sys_stime(addr_t time);
#define CLOCK_REALTIME_ 0
#define CLOCK_MONOTONIC_ 1
#define CLOCK_PROCESS_CPUTIME_ID_ 2
#define CLOCK_REALTIME_COARSE_ 5
slong_t sys_clock_gettime(dword_t clock, addr_t tp);
slong_t sys_clock_settime(dword_t clock, addr_t tp);
slong_t sys_clock_getres(dword_t clock, addr_t res_addr);

// Longs in Linux, so guest longs here: the same layout as before on i386,
// and x86-64's own there.
struct timeval_ {
    time_t_ sec;
    ulong_t usec;
};
struct timespec_ {
    time_t_ sec;
    ulong_t nsec;
};
struct timezone_ {
    dword_t minuteswest;
    dword_t dsttime;
};

static inline clock_t_ clock_from_timeval(struct timeval_ timeval) {
    return timeval.sec * 100 + timeval.usec / 10000;
}

static inline struct timespec convert_timespec(struct timespec_ t) {
    struct timespec ts;
    ts.tv_sec = t.sec;
    ts.tv_nsec = t.nsec;
    return ts;
}

static inline struct timespec convert_timeval(struct timeval_ t) {
    struct timespec ts;
    ts.tv_sec = t.sec;
    ts.tv_nsec = t.usec * 1000;
    return ts;
}

#define ITIMER_REAL_ 0
#define ITIMER_VIRTUAL_ 1
#define ITIMER_PROF_ 2
struct itimerval_ {
    struct timeval_ interval;
    struct timeval_ value;
};

struct itimerspec_ {
    struct timespec_ interval;
    struct timespec_ value;
};

struct tms_ {
    clock_t_ tms_utime;  /* user time */
    clock_t_ tms_stime;  /* system time */
    clock_t_ tms_cutime; /* user time of children */
    clock_t_ tms_cstime; /* system time of children */
};

slong_t sys_setitimer(int_t which, addr_t new_val, addr_t old_val);
slong_t sys_alarm(uint_t seconds);
slong_t sys_timer_create(dword_t clock, addr_t sigevent_addr, addr_t timer_addr);
slong_t sys_timer_settime(dword_t timer, int_t flags, addr_t new_value_addr, addr_t old_value_addr);
slong_t sys_timer_delete(dword_t timer_id);
slong_t sys_timerfd_create(int_t clockid, int_t flags);
slong_t sys_timerfd_settime(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr);

slong_t sys_times(addr_t tbuf);
slong_t sys_nanosleep(addr_t req, addr_t rem);
slong_t sys_gettimeofday(addr_t tv, addr_t tz);
slong_t sys_settimeofday(addr_t tv, addr_t tz);


#endif
