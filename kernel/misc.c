#include <string.h>
#include "kernel/calls.h"

#define PRCTL_SET_KEEPCAPS_ 8
#define PRCTL_SET_NAME_ 15

slong_t sys_prctl(dword_t option, ulong_t arg2, ulong_t UNUSED(arg3), ulong_t UNUSED(arg4), ulong_t UNUSED(arg5)) {
    switch (option) {
        case PRCTL_SET_KEEPCAPS_:
            // stub
            return 0;
        case PRCTL_SET_NAME_: {
            char name[16];
            if (user_read_string(arg2, name, sizeof(name) - 1))
                return _EFAULT;
            name[sizeof(name) - 1] = '\0';
            STRACE("prctl(PRCTL_SET_NAME, \"%s\")", name);
            strcpy(current->comm, name);
            return 0;
        }
        default:
            STRACE("prctl(%#x)", option);
            return _EINVAL;
    }
}

#if GUEST_X86_64

#define ARCH_SET_GS_ 0x1001
#define ARCH_SET_FS_ 0x1002
#define ARCH_GET_FS_ 0x1003
#define ARCH_GET_GS_ 0x1004

// The FS and GS bases: FS is where the C library keeps its thread pointer.
slong_t sys_arch_prctl(int_t code, addr_t addr) {
    STRACE("arch_prctl(%#x, %#x)", code, addr);
    struct x86_64_regs *r = &current->cpu.r;
    switch (code) {
        case ARCH_SET_FS_: r->fs_base = addr; return 0;
        case ARCH_SET_GS_: r->gs_base = addr; return 0;
        case ARCH_GET_FS_: return user_put(addr, r->fs_base) ? _EFAULT : 0;
        case ARCH_GET_GS_: return user_put(addr, r->gs_base) ? _EFAULT : 0;
        default: return _EINVAL;
    }
}

#else

slong_t sys_arch_prctl(int_t code, addr_t addr) {
    STRACE("arch_prctl(%#x, %#x)", code, addr);
    return _EINVAL;
}

#endif

#define REBOOT_MAGIC1 0xfee1dead
#define REBOOT_MAGIC2 672274793
#define REBOOT_MAGIC2A 85072278
#define REBOOT_MAGIC2B 369367448
#define REBOOT_MAGIC2C 537993216

#define REBOOT_CMD_CAD_OFF 0
#define REBOOT_CMD_CAD_ON 0x89abcdef

slong_t sys_reboot(int_t magic, int_t magic2, int_t cmd) {
    STRACE("reboot(%#x, %d, %d)", magic, magic2, cmd);
    if (!superuser())
        return _EPERM;
    if (magic != (int) REBOOT_MAGIC1 ||
            (magic2 != REBOOT_MAGIC2 &&
             magic2 != REBOOT_MAGIC2A &&
             magic2 != REBOOT_MAGIC2B &&
             magic2 != REBOOT_MAGIC2C))
        return _EINVAL;

    switch (cmd) {
        case REBOOT_CMD_CAD_ON:
        case REBOOT_CMD_CAD_OFF:
            return 0;
        default:
            return _EPERM;
    }
}
