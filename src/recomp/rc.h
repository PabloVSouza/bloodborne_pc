/* The runtime interface of recompiled game code (docs/RECOMPILATION.md): what the C written by
 * tools/recomp/bbrecomp.c includes, and what the program (the game, tools/recomp/replay.c) gives it.
 *
 * A recompiled function takes the guest CPU state at the function's entry (its return address on
 * top of the guest stack) and returns with cpu->rip at the address it returned to, as the guest
 * code does. Guest registers live in locals inside; the CPU flags are computed lazily (RcFlags):
 * clang drops what no instruction reads. */
#ifndef BB_RECOMP_RC_H
#define BB_RECOMP_RC_H
#include "../cpu/cpu_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BB_RECOMP_API_VERSION 2u
#define BB_RECOMP_INIT "bb_recomp_init"

typedef struct {
    uint32_t version;
    uint64_t image_base;
    /* Runs the guest instruction at cpu->rip (the interpreter); returns the next rip. */
    uint64_t (*step)(BbCpu *cpu);
    /* A call of guest `target` returning to `next` (nothing pushed yet); cpu->rip is `next` after. */
    void (*call)(BbCpu *cpu, uint64_t target, uint64_t next);
    /* A tail call of cpu->rip (the function's return address on top of the stack); cpu->rip is
     * where it returned to after. */
    void (*tail)(BbCpu *cpu);
    /* The rest of the function from cpu->rip in the translator (its return address at entry_rsp);
     * cpu->rip is where it returned to after. */
    void (*bail)(BbCpu *cpu, uint64_t entry_rsp);
} RcApi;

#ifndef BB_CPU_TRACE_H
typedef void (*RcFn)(BbCpu *cpu);
#endif
/* A recompiled function: its image offset and size, and the hash of the code it was generated
 * from (bbcpu_recomp_hash): a function the game's patches changed is left to the translator. */
typedef struct { uint64_t offset, size, hash; RcFn fn; } RcFunction;
/* The library's entry point: keeps `api`, returns its functions. */
typedef const RcFunction *(*RcInit)(const RcApi *api, size_t *count);

/* ---- for generated code ---- */

/* Lazy flags: the last flag-setting operation and its operands, or FK_CPU (in cpu->flags). */
enum { FK_CPU, FK_ADD, FK_SUB, FK_LOGIC, FK_INC, FK_DEC };
static inline uint64_t rc_mask(int size) { return size >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (size * 8)) - 1; }

/* RFLAGS from the lazy state (as the interpreter computes them). */
static inline uint64_t rc_flags(int kind, uint64_t a, uint64_t b, uint64_t r, int size, int saved_cf,
                                uint64_t cpu_flags) {
    if (kind == FK_CPU) return cpu_flags;
    const uint64_t m = rc_mask(size), sign = UINT64_C(1) << (size * 8 - 1);
    a &= m; b &= m; r &= m;
    uint64_t f = cpu_flags & ~(uint64_t)F_ARITH;
    int cf = 0, of = 0, af = 0;
    switch (kind) {
    case FK_ADD: cf = r < a; of = ((a ^ r) & (b ^ r) & sign) != 0; af = ((a ^ b ^ r) & 0x10) != 0; break;
    case FK_SUB: cf = a < b; of = ((a ^ b) & (a ^ r) & sign) != 0; af = ((a ^ b ^ r) & 0x10) != 0; break;
    case FK_INC: cf = saved_cf; of = r == sign; af = ((a ^ 1 ^ r) & 0x10) != 0; break;
    case FK_DEC: cf = saved_cf; of = r == sign - 1; af = ((a ^ 1 ^ r) & 0x10) != 0; break;
    default: break; /* FK_LOGIC: CF, OF, AF clear */
    }
    if (cf) f |= F_CF;
    if (of) f |= F_OF;
    if (af) f |= F_AF;
    if (!r) f |= F_ZF;
    if (r & sign) f |= F_SF;
    if (!__builtin_parity((unsigned)(r & 0xff))) f |= F_PF;
    return f;
}

/* Condition code cc (0 o, 1 no, 2 b, 3 nb, 4 z, ... 15 nle) of the lazy state. */
static inline int rc_cond(int cc, int kind, uint64_t a, uint64_t b, uint64_t r, int size, int saved_cf,
                          uint64_t cpu_flags) {
    const uint64_t f = rc_flags(kind, a, b, r, size, saved_cf, cpu_flags);
    const int cf = !!(f & F_CF), zf = !!(f & F_ZF), sf = !!(f & F_SF), of = !!(f & F_OF), pf = !!(f & F_PF);
    int v;
    switch (cc >> 1) {
    case 0: v = of; break;
    case 1: v = cf; break;
    case 2: v = zf; break;
    case 3: v = cf || zf; break;
    case 4: v = sf; break;
    case 5: v = pf; break;
    case 6: v = sf != of; break;
    default: v = zf || sf != of; break;
    }
    return (cc & 1) ? !v : v;
}

/* Guest memory: x86 ordering between threads (bb_load/bb_store: acquire/release); the stack is
 * the thread's own (plain accesses). */
static inline uint64_t rc_ld(uint64_t address, int size) { return bb_load(address, size); }
static inline void rc_st(uint64_t address, int size, uint64_t value) { bb_store(address, size, value); }
static inline uint64_t rc_lds(uint64_t address, int size) {
    uint64_t v = 0;
    memcpy(&v, (const void *)address, (size_t)size);
    return v;
}
static inline void rc_sts(uint64_t address, int size, uint64_t value) {
    memcpy((void *)address, &value, (size_t)size);
}

#ifdef __cplusplus
}
#endif
#endif
