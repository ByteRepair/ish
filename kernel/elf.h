#ifndef ELF_H
#define ELF_H

#include <assert.h>
#include "misc.h"

#define ELF_MAGIC "\177ELF"
#define ELF_32BIT 1
#define ELF_64BIT 2
#define ELF_LITTLEENDIAN 1
#define ELF_BIGENDIAN 2
#define ELF_LINUX_ABI 3
#define ELF_EXECUTABLE 2
#define ELF_DYNAMIC 3
#define ELF_X86 3
#define ELF_X86_64 62

// The guest's own ELF: ELF32 for i386, ELF64 for x86-64. Field names are
// the same in both, so the loader reads either; the layouts are each
// format's own, ELF64 moving p_flags up and st_value down.
#if GUEST_X86_64
#define ELF_CLASS ELF_64BIT
#define ELF_MACHINE ELF_X86_64
#else
#define ELF_CLASS ELF_32BIT
#define ELF_MACHINE ELF_X86
#endif

#if GUEST_X86_64
struct elf_header {
    uint32_t magic;
    byte_t bitness;
    byte_t endian;
    byte_t elfversion1;
    byte_t abi;
    byte_t abi_version;
    byte_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t elfversion2;
    qword_t entry_point;
    qword_t prghead_off;
    qword_t secthead_off;
    uint32_t flags;
    uint16_t header_size;
    uint16_t phent_size;
    uint16_t phent_count;
    uint16_t shent_size;
    uint16_t shent_count;
    uint16_t sectname_index;
};
#else
struct elf_header {
    uint32_t magic;
    byte_t bitness;
    byte_t endian;
    byte_t elfversion1;
    byte_t abi;
    byte_t abi_version;
    byte_t padding[7];
    uint16_t type; // library or executable or what
    uint16_t machine;
    uint32_t elfversion2;
    dword_t entry_point;
    dword_t prghead_off;
    dword_t secthead_off;
    uint32_t flags;
    uint16_t header_size;
    uint16_t phent_size;
    uint16_t phent_count;
    uint16_t shent_size;
    uint16_t shent_count;
    uint16_t sectname_index;
};
#endif

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6
#define PT_TLS 7
#define PT_NUM 8

#if GUEST_X86_64
struct prg_header {
    uint32_t type;
    uint32_t flags;
    qword_t offset;
    qword_t vaddr;
    qword_t paddr;
    qword_t filesize;
    qword_t memsize;
    qword_t alignment; // must be power of 2
};
#else
struct prg_header {
    uint32_t type;
    dword_t offset;
    dword_t vaddr;
    dword_t paddr;
    dword_t filesize;
    dword_t memsize;
    uint32_t flags;
    dword_t alignment; // must be power of 2
};
#endif

#define PH_R (1 << 2)
#define PH_W (1 << 1)
#define PH_X (1 << 0)

// two guest words each
struct aux_ent {
    ulong_t type;
    ulong_t value;
};

#define AX_PHDR 3
#define AX_PHENT 4
#define AX_PHNUM 5
#define AX_PAGESZ 6
#define AX_BASE 7
#define AX_FLAGS 8
#define AX_ENTRY 9
#define AX_UID 11
#define AX_EUID 12
#define AX_GID 13
#define AX_EGID 14
#define AX_PLATFORM 15
#define AX_HWCAP 16
#define AX_CLKTCK 17
#define AX_SECURE 23
#define AX_RANDOM 25
#define AX_HWCAP2 26
#define AX_EXECFN 31
#define AX_SYSINFO 32
#define AX_SYSINFO_EHDR 33

struct dyn_ent {
    ulong_t tag;
    ulong_t val;
};

#define DT_NULL 0
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6

#if GUEST_X86_64
struct elf_sym {
    uint32_t name;
    byte_t info;
    byte_t other;
    uint16_t shndx;
    qword_t value;
    qword_t size;
};
#else
struct elf_sym {
    uint32_t name;
    dword_t value;
    dword_t size;
    byte_t info;
    byte_t other;
    uint16_t shndx;
};
#endif


// each format's sizes, as the ELF spec gives them
#if GUEST_X86_64
static_assert(sizeof(struct elf_header) == 64, "Elf64_Ehdr");
static_assert(sizeof(struct prg_header) == 56, "Elf64_Phdr");
static_assert(sizeof(struct elf_sym) == 24, "Elf64_Sym");
static_assert(sizeof(struct aux_ent) == 16, "Elf64_auxv_t");
#else
static_assert(sizeof(struct elf_header) == 52, "Elf32_Ehdr");
static_assert(sizeof(struct prg_header) == 32, "Elf32_Phdr");
static_assert(sizeof(struct elf_sym) == 16, "Elf32_Sym");
static_assert(sizeof(struct aux_ent) == 8, "Elf32_auxv_t");
#endif

#endif
