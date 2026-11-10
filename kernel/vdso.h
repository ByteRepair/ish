#ifndef KERNEL_VDSO_H
#define KERNEL_VDSO_H
#include "tools/ptraceomatic-config.h"

extern const char vdso_data[VDSO_PAGES * (1 << 12)] __asm__("vdso_data");
#if !GUEST_X86_64
addr_t vdso_symbol(const char *name);
#endif

#endif
