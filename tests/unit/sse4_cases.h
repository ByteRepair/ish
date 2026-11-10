// The instructions engine_test checks blink against x86 hardware with, and
// what each one runs on. Shared by sse4_oracle.c, which runs them natively
// and writes what they gave to sse4_expected.h, and by engine_test.c, which
// runs them under blink and compares.
//
// Each case is one register-to-register instruction inside the same stub:
// load the state rdi points to, run the instruction, store the state back.
// The inputs are a fixed sequence, the same on both sides: lanes of random
// bytes, of short strings with zeros in them, and of floats and doubles
// chosen to round, compare and multiply at the edges. Every case runs on
// every input, and what the outputs come to is one hash per case.
#ifndef SSE4_CASES_H
#define SSE4_CASES_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct sse4_state {
    uint8_t xmm[3][16];
    uint64_t rax, rcx, rdx, rsi, rflags;
};

struct sse4_case {
    char name[24];
    uint8_t bytes[8];
    uint8_t n;
    uint16_t flags;  // the flags compared: SSE4_FLAGS unless said otherwise
};

#define SSE4_INPUTS 12
#define SSE4_MAX_CASES 1024
// The flags an instruction may write: OF, SF, ZF, AF, PF and CF.
#define SSE4_FLAGS 0x8d5
#define SSE4_ZF_CF 0x41

// The stub around a case. rdi holds the state's address throughout.
static const uint8_t sse4_prologue[] = {
    0xf3, 0x0f, 0x6f, 0x07,        // movdqu xmm0, [rdi]
    0xf3, 0x0f, 0x6f, 0x4f, 0x10,  // movdqu xmm1, [rdi+16]
    0xf3, 0x0f, 0x6f, 0x57, 0x20,  // movdqu xmm2, [rdi+32]
    0x48, 0x8b, 0x47, 0x30,        // mov rax, [rdi+48]
    0x48, 0x8b, 0x4f, 0x38,        // mov rcx, [rdi+56]
    0x48, 0x8b, 0x57, 0x40,        // mov rdx, [rdi+64]
    0x48, 0x8b, 0x77, 0x48,        // mov rsi, [rdi+72]
    0xff, 0x77, 0x50, 0x9d,        // push [rdi+80]; popfq
};
static const uint8_t sse4_epilogue[] = {
    0x9c, 0x8f, 0x47, 0x50,        // pushfq; pop [rdi+80]
    0xf3, 0x0f, 0x7f, 0x07,        // movdqu [rdi], xmm0
    0xf3, 0x0f, 0x7f, 0x4f, 0x10,  // movdqu [rdi+16], xmm1
    0xf3, 0x0f, 0x7f, 0x57, 0x20,  // movdqu [rdi+32], xmm2
    0x48, 0x89, 0x47, 0x30,        // mov [rdi+48], rax
    0x48, 0x89, 0x4f, 0x38,        // mov [rdi+56], rcx
    0x48, 0x89, 0x57, 0x40,        // mov [rdi+64], rdx
    0x48, 0x89, 0x77, 0x48,        // mov [rdi+72], rsi
};

static int sse4_add(struct sse4_case *cases, int n, const char *name, const uint8_t *bytes, int len) {
    struct sse4_case *c = &cases[n];
    snprintf(c->name, sizeof(c->name), "%s", name);
    memcpy(c->bytes, bytes, len);
    c->n = len;
    c->flags = SSE4_FLAGS;
    return n + 1;
}

// 66 [REX] 0F 38/3A op modrm [imm8]
static int sse4_op(struct sse4_case *cases, int n, const char *name, uint8_t rex, uint8_t map, uint8_t op,
        uint8_t modrm, int imm) {
    uint8_t b[8];
    int len = 0;
    b[len++] = 0x66;
    if (rex)
        b[len++] = rex;
    b[len++] = 0x0f;
    b[len++] = map;
    b[len++] = op;
    b[len++] = modrm;
    char full[24];
    if (imm >= 0) {
        b[len++] = imm;
        snprintf(full, sizeof(full), "%s/%02x", name, imm);
    } else {
        snprintf(full, sizeof(full), "%s", name);
    }
    return sse4_add(cases, n, full, b, len);
}

// Every case, in a fixed order; returns how many.
static int sse4_cases(struct sse4_case *cases) {
    int n = 0;
    // reg xmm1, r/m xmm2; reg xmm1, r/m rax
    enum { XX = 0xca, XR = 0xc8 };
    static const struct { const char *name; uint8_t op; } map2[] = {
        {"pblendvb", 0x10}, {"blendvps", 0x14}, {"blendvpd", 0x15}, {"ptest", 0x17},
        {"pmovsxbw", 0x20}, {"pmovsxbd", 0x21}, {"pmovsxbq", 0x22}, {"pmovsxwd", 0x23},
        {"pmovsxwq", 0x24}, {"pmovsxdq", 0x25}, {"pmuldq", 0x28}, {"pcmpeqq", 0x29},
        {"packusdw", 0x2b}, {"pmovzxbw", 0x30}, {"pmovzxbd", 0x31}, {"pmovzxbq", 0x32},
        {"pmovzxwd", 0x33}, {"pmovzxwq", 0x34}, {"pmovzxdq", 0x35}, {"pcmpgtq", 0x37},
        {"pminsb", 0x38}, {"pminsd", 0x39}, {"pminuw", 0x3a}, {"pminud", 0x3b},
        {"pmaxsb", 0x3c}, {"pmaxsd", 0x3d}, {"pmaxuw", 0x3e}, {"pmaxud", 0x3f},
        {"pmulld", 0x40}, {"phminposuw", 0x41},
    };
    for (size_t i = 0; i < sizeof(map2) / sizeof(*map2); i++) {
        n = sse4_op(cases, n, map2[i].name, 0, 0x38, map2[i].op, XX, -1);
        // The SDM has ptest clear AF, OF, PF and SF; Rosetta 2 leaves them
        // as they were, so on Apple silicon only ZF and CF are the CPU's.
        if (map2[i].op == 0x17)
            cases[n - 1].flags = SSE4_ZF_CF;
    }

    static const uint8_t rounds[] = {0, 1, 2, 3, 4, 8, 9, 0xb};
    static const char *round_names[] = {"roundps", "roundpd", "roundss", "roundsd"};
    for (int op = 0; op < 4; op++)
        for (size_t i = 0; i < sizeof(rounds); i++)
            n = sse4_op(cases, n, round_names[op], 0, 0x3a, 0x08 + op, XX, rounds[i]);

    static const uint8_t blends[] = {0x00, 0x05, 0x0a, 0x0f, 0x5a, 0xa5, 0xff};
    for (size_t i = 0; i < sizeof(blends); i++) {
        n = sse4_op(cases, n, "blendps", 0, 0x3a, 0x0c, XX, blends[i] & 0xf);
        n = sse4_op(cases, n, "blendpd", 0, 0x3a, 0x0d, XX, blends[i] & 0x3);
        n = sse4_op(cases, n, "pblendw", 0, 0x3a, 0x0e, XX, blends[i]);
    }

    static const uint8_t lanes[] = {0, 1, 3, 5, 7, 15, 0x13};
    for (size_t i = 0; i < sizeof(lanes); i++) {
        n = sse4_op(cases, n, "pextrb", 0, 0x3a, 0x14, XR, lanes[i]);
        n = sse4_op(cases, n, "pextrw", 0, 0x3a, 0x15, XR, lanes[i]);
        n = sse4_op(cases, n, "pextrd", 0, 0x3a, 0x16, XR, lanes[i]);
        n = sse4_op(cases, n, "pextrq", 0x48, 0x3a, 0x16, XR, lanes[i]);
        n = sse4_op(cases, n, "extractps", 0, 0x3a, 0x17, XR, lanes[i]);
        n = sse4_op(cases, n, "pinsrb", 0, 0x3a, 0x20, XR, lanes[i]);
        n = sse4_op(cases, n, "pinsrd", 0, 0x3a, 0x22, XR, lanes[i]);
        n = sse4_op(cases, n, "pinsrq", 0x48, 0x3a, 0x22, XR, lanes[i]);
    }
    static const uint8_t insertps[] = {0x00, 0x1d, 0x4e, 0x63, 0x90, 0xf8, 0xff};
    for (size_t i = 0; i < sizeof(insertps); i++)
        n = sse4_op(cases, n, "insertps", 0, 0x3a, 0x21, XX, insertps[i]);

    static const uint8_t dots[] = {0xff, 0xf1, 0x3c, 0x7e, 0x33, 0x31, 0x12, 0x00};
    for (size_t i = 0; i < sizeof(dots); i++) {
        n = sse4_op(cases, n, "dpps", 0, 0x3a, 0x40, XX, dots[i]);
        n = sse4_op(cases, n, "dppd", 0, 0x3a, 0x41, XX, dots[i]);
    }
    for (int imm = 0; imm < 8; imm++)
        n = sse4_op(cases, n, "mpsadbw", 0, 0x3a, 0x42, XX, imm);

    // every mode: format, aggregation, polarity, and which end or a mask
    static const char *strings[] = {"pcmpestrm", "pcmpestri", "pcmpistrm", "pcmpistri"};
    for (int op = 0; op < 4; op++)
        for (int imm = 0; imm < 0x80; imm++)
            n = sse4_op(cases, n, strings[op], 0, 0x3a, 0x60 + op, XX, imm);
    static const uint8_t wide[] = {0x00, 0x0c, 0x41, 0x18};
    for (size_t i = 0; i < sizeof(wide); i++) {
        n = sse4_op(cases, n, "pcmpestri.w", 0x48, 0x3a, 0x61, XX, wide[i]);
        n = sse4_op(cases, n, "pcmpestrm.w", 0x48, 0x3a, 0x60, XX, wide[i]);
    }

    // crc32 eax/rax with dl, dx, edx, rdx; popcnt of edx, dx, rdx
    n = sse4_add(cases, n, "crc32b", (const uint8_t[]) {0xf2, 0x0f, 0x38, 0xf0, 0xc2}, 5);
    n = sse4_add(cases, n, "crc32w", (const uint8_t[]) {0x66, 0xf2, 0x0f, 0x38, 0xf1, 0xc2}, 6);
    n = sse4_add(cases, n, "crc32d", (const uint8_t[]) {0xf2, 0x0f, 0x38, 0xf1, 0xc2}, 5);
    n = sse4_add(cases, n, "crc32q", (const uint8_t[]) {0xf2, 0x48, 0x0f, 0x38, 0xf1, 0xc2}, 6);
    n = sse4_add(cases, n, "popcntd", (const uint8_t[]) {0xf3, 0x0f, 0xb8, 0xc2}, 4);
    n = sse4_add(cases, n, "popcntw", (const uint8_t[]) {0x66, 0xf3, 0x0f, 0xb8, 0xc2}, 5);
    n = sse4_add(cases, n, "popcntq", (const uint8_t[]) {0xf3, 0x48, 0x0f, 0xb8, 0xc2}, 5);

    // SSE2's compares, whose predicate results are masks
    for (int imm = 0; imm < 8; imm++) {
        char name[24];
        snprintf(name, sizeof(name), "cmpps/%d", imm);
        n = sse4_add(cases, n, name, (const uint8_t[]) {0x0f, 0xc2, XX, imm}, 4);
        snprintf(name, sizeof(name), "cmppd/%d", imm);
        n = sse4_add(cases, n, name, (const uint8_t[]) {0x66, 0x0f, 0xc2, XX, imm}, 5);
        snprintf(name, sizeof(name), "cmpss/%d", imm);
        n = sse4_add(cases, n, name, (const uint8_t[]) {0xf3, 0x0f, 0xc2, XX, imm}, 5);
        snprintf(name, sizeof(name), "cmpsd/%d", imm);
        n = sse4_add(cases, n, name, (const uint8_t[]) {0xf2, 0x0f, 0xc2, XX, imm}, 5);
    }
    return n;
}

static uint64_t sse4_next(uint64_t *seed) {
    // xorshift64*: the same sequence everywhere
    *seed ^= *seed >> 12;
    *seed ^= *seed << 25;
    *seed ^= *seed >> 27;
    return *seed * 0x2545f4914f6cdd1dull;
}

// Input i for every case. Inputs 0-2 are random bytes; 3-5 short strings,
// their bytes and words from a small alphabet with zero among it, and lengths
// in rax and rdx around 16 either way; 6-8 floats and 9-11 doubles from
// values that sit on rounding boundaries, signed zeros, infinities and NaNs.
static void sse4_input(int i, struct sse4_state *s) {
    uint64_t seed = 0x9e3779b97f4a7c15ull * (i + 1);
    static const uint8_t alphabet[] = {0, 'a', 'b', 'c', 'a', 'b', 0x80, 0xff};
    static const float floats[] = {
        1.5f, -2.5f, 0.5f, -0.5f, 2.5f, 3.75f, -0.0f, 0.0f, 1e10f, -7.25f,
        1.0f / 3, 8388609.0f, __builtin_inff(), -__builtin_inff(), __builtin_nanf(""), 1.0f,
    };
    static const double doubles[] = {
        1.5, -2.5, 0.5, -0.5, 2.5, 3.75, -0.0, 0.0, 1e300, -7.25,
        1.0 / 3, 4503599627370497.0, __builtin_inf(), -__builtin_inf(), __builtin_nan(""), 1.0,
    };
    for (int r = 0; r < 3; r++) {
        for (int b = 0; b < 16; b++) {
            uint64_t x = sse4_next(&seed);
            switch (i / 3) {
                case 0:
                    s->xmm[r][b] = x;
                    break;
                case 1:
                    s->xmm[r][b] = alphabet[x % sizeof(alphabet)];
                    break;
                case 2:
                    if (b % 4 == 0)
                        memcpy(&s->xmm[r][b], &floats[x % 16], 4);
                    break;
                default:
                    if (b % 8 == 0)
                        memcpy(&s->xmm[r][b], &doubles[x % 16], 8);
                    break;
            }
        }
    }
    s->rax = i / 3 == 1 ? (uint64_t) (int64_t) (sse4_next(&seed) % 41 - 20) : sse4_next(&seed);
    s->rdx = i / 3 == 1 ? (uint64_t) (int64_t) (sse4_next(&seed) % 41 - 20) : sse4_next(&seed);
    s->rcx = sse4_next(&seed);
    s->rsi = sse4_next(&seed);
    s->rflags = 0x2 | (sse4_next(&seed) & SSE4_FLAGS);
}

// What a case's outputs come to, over its inputs in order.
static uint64_t sse4_hash(uint64_t hash, const struct sse4_case *c, const struct sse4_state *s) {
    struct sse4_state t = *s;
    t.rflags &= c->flags;
    const uint8_t *p = (const uint8_t *) &t;
    for (size_t i = 0; i < sizeof(t); i++)
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    return hash;
}
#define SSE4_HASH_SEED 0xcbf29ce484222325ull

static void sse4_print(const char *who, int i, const struct sse4_case *c, const struct sse4_state *s) {
    printf("  %s %2d:", who, i);
    for (int r = 0; r < 3; r++) {
        printf(" xmm%d=", r);
        for (int b = 15; b >= 0; b--)
            printf("%02x", s->xmm[r][b]);
    }
    printf(" rax=%016llx rcx=%016llx rdx=%016llx rsi=%016llx flags=%03llx\n",
            (unsigned long long) s->rax, (unsigned long long) s->rcx, (unsigned long long) s->rdx,
            (unsigned long long) s->rsi, (unsigned long long) (s->rflags & c->flags));
}

#endif
