#ifndef FS_STAT_H
#define FS_STAT_H

#include <assert.h>
#include "misc.h"

struct statbuf {
    qword_t dev;
    qword_t inode;
    dword_t mode;
    dword_t nlink;
    dword_t uid;
    dword_t gid;
    qword_t rdev;
    qword_t size;
    dword_t blksize;
    qword_t blocks;
    dword_t atime;
    dword_t atime_nsec;
    dword_t mtime;
    dword_t mtime_nsec;
    dword_t ctime;
    dword_t ctime_nsec;
};

struct oldstat {
    word_t dev;
    word_t ino;
    word_t mode;
    word_t nlink;
    word_t uid;
    word_t gid;
    word_t rdev;
    uint_t size;
    uint_t atime;
    uint_t mtime;
    uint_t ctime;
};

struct newstat {
    dword_t dev;
    dword_t ino;
    word_t mode;
    word_t nlink;
    word_t uid;
    word_t gid;
    dword_t rdev;
    dword_t size;
    dword_t blksize;
    dword_t blocks;
    dword_t atime;
    dword_t atime_nsec;
    dword_t mtime;
    dword_t mtime_nsec;
    dword_t ctime;
    dword_t ctime_nsec;
    char pad[8];
};

struct newstat64 {
    qword_t dev;
    dword_t _pad1;
    dword_t fucked_ino;
    dword_t mode;
    dword_t nlink;
    dword_t uid;
    dword_t gid;
    qword_t rdev;
    dword_t _pad2;
    qword_t size;
    dword_t blksize;
    qword_t blocks;
    dword_t atime;
    dword_t atime_nsec;
    dword_t mtime;
    dword_t mtime_nsec;
    dword_t ctime;
    dword_t ctime_nsec;
    qword_t ino;
} __attribute__((packed));

// x86-64's struct stat, which is not i386's stat64 widened: the fields are
// in another order, nlink is a long, and there is no second inode.
struct stat_x86_64 {
    qword_t dev;
    qword_t ino;
    qword_t nlink;
    dword_t mode;
    dword_t uid;
    dword_t gid;
    dword_t _pad0;
    qword_t rdev;
    sqword_t size;
    sqword_t blksize;
    sqword_t blocks;
    qword_t atime;
    qword_t atime_nsec;
    qword_t mtime;
    qword_t mtime_nsec;
    qword_t ctime;
    qword_t ctime_nsec;
    sqword_t _unused[3];
};
static_assert(sizeof(struct stat_x86_64) == 144, "x86-64 struct stat");

// What stat, fstat, lstat and fstatat write for this guest.
#if GUEST_X86_64
typedef struct stat_x86_64 guest_stat_t;
#else
typedef struct newstat64 guest_stat_t;
#endif

struct statfsbuf {
    long type;
    long bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint64_t fsid;
    long namelen;
    long frsize;
    long flags;
    long spare[4];
};

// Longs in Linux: x86-64's statfs is this with them widened.
struct statfs_ {
    ulong_t type;
    ulong_t bsize;
    ulong_t blocks;
    ulong_t bfree;
    ulong_t bavail;
    ulong_t files;
    ulong_t ffree;
    uint64_t fsid;
    ulong_t namelen;
    ulong_t frsize;
    ulong_t flags;
    ulong_t spare[4];
} __attribute__((packed));

struct statfs64_ {
    uint_t type;
    uint_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint64_t fsid;
    uint_t namelen;
    uint_t frsize;
    uint_t flags;
    uint_t pad[4];
} __attribute__((packed));

struct statx_timestamp_ {
    int64_t sec;
    uint32_t nsec;
    uint32_t _pad;
};

struct statx_ {
    uint32_t mask;
    uint32_t blksize;
    uint64_t attributes;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t _pad1;
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributes_mask;
    struct statx_timestamp_ atime;
    struct statx_timestamp_ btime;
    struct statx_timestamp_ ctime;
    struct statx_timestamp_ mtime;
    uint32_t rdev_major;
    uint32_t rdev_minor;
    uint32_t dev_major;
    uint32_t dev_minor;
    uint64_t mnt_id;
    uint32_t dio_mem_align;
    uint32_t dio_offset_align;
    uint32_t _pad2[24];
} __attribute__((packed));

#define STATX_BASIC_STATS_ 0x7ff

#endif
