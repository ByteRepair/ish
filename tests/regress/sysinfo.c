// sysinfo's memory is totalram times mem_unit, in bytes where the guest's
// long holds them and in pages where it does not. A zero mem_unit says the
// machine has no memory, and JavaScriptCore, which sizes its heap from it,
// then collects all but constantly.
#include "sys.h"

struct sysinfo_ {
    slong uptime;
    ulong loads[3];
    ulong totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    ulong totalhigh, freehigh;
    unsigned mem_unit;
    char room[32];  // Linux's padding after, which differs by ABI
};

void _start(void) {
    struct sysinfo_ info;
    check(sys1(NR_sysinfo, &info) == 0, "sysinfo failed");
    check(info.mem_unit != 0, "mem_unit is 0, so there is no memory");
    unsigned long long total = (unsigned long long) info.totalram * info.mem_unit;
    check(total >= 64ull << 20, "less than 64 MiB of memory");
    check(info.freeram <= info.totalram, "more memory free than there is");
    finish(1, 0);
}
