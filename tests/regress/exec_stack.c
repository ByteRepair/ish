// What exec leaves for a program's first instruction: a 16-byte aligned
// stack holding argc, the argv and envp pointer arrays each ending in NULL,
// and the auxiliary vector after them, every entry a pair of guest words.
// The values checked are the ones the loader cannot get wrong without a
// program noticing: the page size, the entry point, the program headers'
// location, the platform string, the random bytes' address.
#include "sys.h"

enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PAGESZ = 6, AT_ENTRY = 9,
       AT_PLATFORM = 15, AT_RANDOM = 25, AT_EXECFN = 31, AT_SYSINFO_EHDR = 33 };

void _start(void);

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

__attribute__((used)) static void start_c(ulong *sp) {
    check(((ulong) sp & 15) == 0, "the stack is not 16-byte aligned at entry");
    ulong argc = sp[0];
    char **argv = (char **) &sp[1];
    check(argc == 1, "argc is not 1");
    check(argv[1] == 0, "argv is not NULL-terminated after argc entries");
    check(streq(argv[0], "/tmp/regress/exec_stack"), "argv[0] is not the program's path");
    char **envp = &argv[argc + 1];
    while (*envp)
        envp++;
    ulong *auxv = (ulong *) (envp + 1);

    ulong pagesz = 0, entry = 0, phdr = 0, phent = 0, random = 0;
    const char *platform = 0, *execfn = 0;
    int sysinfo_ehdr = 0;
    for (; auxv[0] != AT_NULL; auxv += 2) {
        switch (auxv[0]) {
            case AT_PAGESZ: pagesz = auxv[1]; break;
            case AT_ENTRY: entry = auxv[1]; break;
            case AT_PHDR: phdr = auxv[1]; break;
            case AT_PHENT: phent = auxv[1]; break;
            case AT_RANDOM: random = auxv[1]; break;
            case AT_PLATFORM: platform = (const char *) auxv[1]; break;
            case AT_EXECFN: execfn = (const char *) auxv[1]; break;
            case AT_SYSINFO_EHDR: sysinfo_ehdr = 1; break;
        }
    }
    check(pagesz == 4096, "AT_PAGESZ is not 4096");
    check(entry == (ulong) _start, "AT_ENTRY is not _start");
    check(random != 0, "AT_RANDOM is missing");
    check(execfn && streq(execfn, "/tmp/regress/exec_stack"), "AT_EXECFN is not the program's path");
    // the program headers are where AT_PHDR says: the first is this file's
    // own PT_LOAD or PT_PHDR, and each is the size AT_PHENT gives
    check(phdr != 0, "AT_PHDR is missing");
#if defined(__x86_64__)
    check(phent == 56, "AT_PHENT is not an Elf64_Phdr's size");
    check(platform && streq(platform, "x86_64"), "AT_PLATFORM is not x86_64");
    check(!sysinfo_ehdr, "x86-64 has a vdso it should not have yet");
#else
    check(phent == 32, "AT_PHENT is not an Elf32_Phdr's size");
    check(platform && streq(platform, "i686"), "AT_PLATFORM is not i686");
    check(sysinfo_ehdr, "i386 has no vdso");
#endif
    unsigned type = *(unsigned *) phdr;
    check(type == 1 || type == 6, "AT_PHDR does not point at a program header");
    finish(1, 0);
}

// The stack pointer as exec left it, before any prologue can move it.
#if defined(__x86_64__)
__asm__(".globl _start\n_start:\n\tmov %rsp, %rdi\n\tcall start_c\n");
#else
__asm__(".globl _start\n_start:\n\tmov %esp, %eax\n\tsub $12, %esp\n\tpush %eax\n\tcall start_c\n");
#endif
