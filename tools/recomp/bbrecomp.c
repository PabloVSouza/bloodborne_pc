/* tools/recomp/bbrecomp.c ELF FUNCTIONS_TSV OUT.c OFFSET...: the recompiler (docs/RECOMPILATION.md). Writes
 * one C function per game function (image offsets; sizes from tools/recomp/scan.c's functions.tsv)
 * against src/recomp/rc.h, in files OUT_<n>.c, and the library's table and entry point in OUT.c.
 * Build: tools/recomp/recomp.sh.
 *
 * Generated code keeps the guest registers in a local array and the flags lazily; every
 * instruction gets a label. Instructions the generator does not translate run in the interpreter
 * (rc->step) with the state spilled around them; a branch to an unknown place goes back to the
 * translator (rc->bail) or, out of the function, is a tail call (rc->tail). */
#include "../../src/cpu/trace.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define M(name) ZYDIS_MNEMONIC_##name

void *runtime_low_map(size_t size, int prot) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
uint64_t bb_image_base;

/* ---- the image ---- */

typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_align; } Elf64_Phdr;

static uint64_t base; /* where the image is mapped here */

static void map_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) exit(1);
    fclose(f);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    uint64_t end = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == 1 && ph->p_vaddr + ph->p_memsz > end) end = ph->p_vaddr + ph->p_memsz;
    }
    uint8_t *image = mmap(NULL, (end + 16383) & ~UINT64_C(16383), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (image == MAP_FAILED) exit(1);
    base = (uint64_t)(uintptr_t)image;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != 1) continue;
        memcpy(image + ph->p_vaddr, data + ph->p_offset, ph->p_filesz);
        if (ph->p_flags & 1) bbcpu_add_guest_code(base + ph->p_vaddr, ph->p_memsz);
    }
    free(data);
}

/* ---- functions ---- */

typedef struct { uint64_t offset, size; } Function;
static Function *functions;
static size_t function_count;

static uint64_t size_of(const char *tsv, uint64_t offset) {
    static uint64_t *starts, *sizes;
    static size_t count;
    if (!starts) {
        FILE *f = fopen(tsv, "r");
        if (!f) { perror(tsv); exit(1); }
        char line[256];
        size_t capacity = 0;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] != '0') continue;
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 1 << 18;
                starts = realloc(starts, capacity * 8);
                sizes = realloc(sizes, capacity * 8);
            }
            char *end;
            starts[count] = strtoull(line, &end, 16);
            sizes[count++] = strtoull(end, NULL, 10);
        }
        fclose(f);
    }
    for (size_t i = 0; i < count; ++i)
        if (starts[i] == offset) return sizes[i];
    return 0;
}

/* ---- output ---- */

static FILE *out;
static void emit(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
}

/* Closes `out` where writing ended: a translation that gave up may have written past it. */
static void close_output(void) {
    fflush(out);
    ftruncate(fileno(out), ftell(out));
    fclose(out);
}

#define SPILL "memcpy(c->r, r, sizeof(r));"
#define RELOAD "memcpy(r, c->r, sizeof(r));"
#define COND "rc_cond(%d, fk, fa, fb, fr, fz, fcf, c->flags)"
#define FLAGS_OUT "c->flags = rc_flags(fk, fa, fb, fr, fz, fcf, c->flags); fk = FK_CPU;"

typedef struct { char s[1024]; } Str;
static Str str(const char *format, ...) __attribute__((format(printf, 1, 2)));
static Str str(const char *format, ...) {
    Str v;
    va_list args;
    va_start(args, format);
    vsnprintf(v.s, sizeof(v.s), format, args);
    va_end(args);
    return v;
}
static uint64_t mask(int size) { return size >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (size * 8)) - 1; }

/* The address of a memory operand (image offsets for rip-relative); 0: not translatable. */
static int address(uint64_t off, const BbInsn *in, const BbOp *op, Str *s, int *stack) {
    if (op->segment) return 0;
    char text[200] = "";
    size_t n = 0;
    if (op->base == 0xfe) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "(B + 0x%" PRIx64 "ull)", off + in->length + (uint64_t)op->disp);
    } else {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "(0x%" PRIx64 "ull", (uint64_t)op->disp);
        if (op->base != 0xff) n += (size_t)snprintf(text + n, sizeof(text) - n, " + r[%d]", op->base);
        if (op->index != 0xff) n += (size_t)snprintf(text + n, sizeof(text) - n, " + r[%d] * %d", op->index, op->scale);
        n += (size_t)snprintf(text + n, sizeof(text) - n, ")");
    }
    *stack = op->base == RSP && op->index == 0xff;
    *s = str("%s", text);
    return 1;
}

static int gpr(const BbOp *op) { return op->type == OP_REG && op->kind == RK_GPR; }

/* An operand's value, zero-extended (immediates: sign-extended to `size`, then masked). */
static int read_op(uint64_t off, const BbInsn *in, const BbOp *op, int size, Str *v) {
    if (gpr(op)) {
        if (op->size == 1 && op->high8) *v = str("((r[%d] >> 8) & 0xff)", op->reg);
        else if (op->size == 8) *v = str("r[%d]", op->reg);
        else *v = str("(r[%d] & 0x%" PRIx64 "ull)", op->reg, mask(op->size));
        return 1;
    }
    if (op->type == OP_IMM) {
        *v = str("0x%" PRIx64 "ull", (uint64_t)op->disp & mask(size));
        return 1;
    }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *v = str("%s(%s, %d)", stack ? "rc_lds" : "rc_ld", a.s, op->size);
        return 1;
    }
    return 0;
}

/* A statement writing `v` to a register or memory operand. */
static int write_op(uint64_t off, const BbInsn *in, const BbOp *op, const char *v, Str *s) {
    if (gpr(op)) {
        const int x = op->reg;
        switch (op->size) {
        case 8: *s = str("r[%d] = (%s);", x, v); break;
        case 4: *s = str("r[%d] = (uint32_t)(%s);", x, v); break;
        case 2: *s = str("r[%d] = (r[%d] & ~0xffffull) | ((%s) & 0xffff);", x, x, v); break;
        default:
            if (op->high8) *s = str("r[%d] = (r[%d] & ~0xff00ull) | (((%s) & 0xff) << 8);", x, x, v);
            else *s = str("r[%d] = (r[%d] & ~0xffull) | ((%s) & 0xff);", x, x, v);
        }
        return 1;
    }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *s = str("%s(%s, %d, (%s));", stack ? "rc_sts" : "rc_st", a.s, op->size, v);
        return 1;
    }
    return 0;
}

static int cc_of(int m) {
    static const int jcc[16] = {M(JO), M(JNO), M(JB), M(JNB), M(JZ), M(JNZ), M(JBE), M(JNBE),
                                M(JS), M(JNS), M(JP), M(JNP), M(JL), M(JNL), M(JLE), M(JNLE)};
    static const int setcc[16] = {M(SETO), M(SETNO), M(SETB), M(SETNB), M(SETZ), M(SETNZ), M(SETBE),
                                  M(SETNBE), M(SETS), M(SETNS), M(SETP), M(SETNP), M(SETL), M(SETNL),
                                  M(SETLE), M(SETNLE)};
    static const int cmovcc[16] = {M(CMOVO), M(CMOVNO), M(CMOVB), M(CMOVNB), M(CMOVZ), M(CMOVNZ),
                                   M(CMOVBE), M(CMOVNBE), M(CMOVS), M(CMOVNS), M(CMOVP), M(CMOVNP),
                                   M(CMOVL), M(CMOVNL), M(CMOVLE), M(CMOVNLE)};
    for (int i = 0; i < 16; ++i) {
        if (jcc[i] == m) return i;
        if (setcc[i] == m) return 16 + i;
        if (cmovcc[i] == m) return 32 + i;
    }
    return -1;
}

typedef struct { uint64_t start, end; } Range;

/* The functions being recompiled, by offset: calls of them go through their direct-call slot
 * (d_<offset>, rc.h) when the program filled it. */
static uint64_t *recompiled;
static int by_offset(const void *a, const void *b);
static int is_recompiled(uint64_t t) {
    return recompiled && bsearch(&t, recompiled, function_count, sizeof(*recompiled), by_offset) != NULL;
}

/* A jump to image offset `t`: a goto inside the function. Out of it: a tail call when the stack is
 * back at the entry (the return address on top), else code of this function elsewhere (a part the
 * compiler moved out, a shared block): the translator runs the rest, the function's return address
 * at entry_rsp (rc->bail). */
static void jump(Range f, uint64_t t) {
    if (t >= f.start && t < f.end) emit("goto L_%" PRIx64 ";", t);
    else if (is_recompiled(t))
        emit("{ extern RcFn d_%" PRIx64 "; " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; if (r[RSP] == entry_rsp) { if (d_%" PRIx64 ") d_%" PRIx64 "(c); else rc->tail(c); } else rc->bail(c, entry_rsp); return; }", t, t, t, t);
    else emit("{ " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; if (r[RSP] == entry_rsp) rc->tail(c); else rc->bail(c, entry_rsp); return; }", t);
}

static int translate_vector(uint64_t off, const BbInsn *in);
static int translate_shift_mul(uint64_t off, const BbInsn *in);
static int translate_extra(uint64_t off, const BbInsn *in);
static int translate_atomic(uint64_t off, const BbInsn *in);

/* One instruction natively; 0: it goes to the interpreter. */
static int translate(Range f, uint64_t off, const BbInsn *in, uint64_t *stats) {
    const uint64_t next = off + in->length;
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0], *s = &in->op[1];
    Str a, b, w;
    if (in->rep || in->repne) return 0;
    if (in->lock || m == M(XADD) || m == M(CMPXCHG)) return translate_atomic(off, in);
    const int cc = cc_of(m);
    if (cc >= 0 && cc < 16) { /* jcc */
        if (d->type != OP_IMM) return 0;
        emit("if (" COND ") ", cc);
        jump(f, off + (uint64_t)d->disp);
        emit("\n");
        return 1;
    }
    if (cc >= 16 && cc < 32) { /* setcc */
        if (!write_op(off, in, d, str(COND, cc - 16).s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    if (cc >= 32) { /* cmovcc */
        if (!gpr(d) || !read_op(off, in, s, d->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("if (" COND ") { %s }", cc - 32, w.s);
        if (d->size == 4) emit(" else { r[%d] = (uint32_t)r[%d]; }", d->reg, d->reg);
        emit("\n");
        return 1;
    }
    if (bbcpu_is_tcb_load(in)) { /* mov reg, fs:[0] (the loader makes it a gs load): the TCB */
        emit("r[%d] = c->tcb;\n", d->reg);
        return 1;
    }
    switch (m) {
    case M(NOP): case M(ENDBR64): case M(PAUSE): case M(PREFETCHNTA): case M(PREFETCHT0):
    case M(PREFETCHT1): case M(PREFETCHT2): case M(PREFETCHW):
        emit("/* %s */\n", ZydisMnemonicGetString((ZydisMnemonic)m));
        return 1;
    case M(MOV):
        if (d->type == OP_REG && d->kind != RK_GPR) return 0;
        if (s->type == OP_REG && s->kind != RK_GPR) return 0;
        if (!read_op(off, in, s, d->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    case M(MOVZX):
        if (!gpr(d) || !read_op(off, in, s, s->size, &b) || !write_op(off, in, d, b.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    case M(MOVSX): case M(MOVSXD): {
        if (!gpr(d) || !read_op(off, in, s, s->size, &b)) return 0;
        static const char *const ext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
        if (!ext[s->size]) return 0;
        const Str v = str("(uint64_t)(int64_t)(%s)(%s)", ext[s->size], b.s);
        if (!write_op(off, in, d, v.s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    case M(LEA): {
        int stack;
        if (!gpr(d) || s->type != OP_AGEN || !address(off, in, s, &a, &stack)) return 0;
        if (!write_op(off, in, d, str("%s & 0x%" PRIx64 "ull", a.s, mask(d->size)).s, &w)) return 0;
        emit("%s\n", w.s);
        return 1;
    }
    case M(ADD): case M(SUB): case M(CMP): case M(AND): case M(OR): case M(XOR): case M(TEST): {
        const int size = d->size;
        if (!(gpr(d) || d->type == OP_MEM)) return 0;
        if (!read_op(off, in, s, size, &b)) return 0;
        const int kind = m == M(ADD) ? 1 : (m == M(SUB) || m == M(CMP)) ? 2 : 3;
        const char *op = m == M(ADD) ? "+" : (m == M(SUB) || m == M(CMP)) ? "-" : (m == M(OR)) ? "|" : (m == M(XOR)) ? "^" : "&";
        emit("{ ");
        Str da;
        int stack = 0;
        if (d->type == OP_MEM) { /* read-modify-write: the address once */
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        }
        emit("const uint64_t b = %s; const uint64_t v = (a %s b) & 0x%" PRIx64 "ull; ", b.s, op, mask(size));
        emit("fk = %s; fa = a; fb = b; fr = v; fz = %d; ", kind == 1 ? "FK_ADD" : kind == 2 ? "FK_SUB" : "FK_LOGIC", size);
        if (m != M(CMP) && m != M(TEST)) {
            if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
            else {
                if (!write_op(off, in, d, "v", &w)) return 0;
                emit("%s", w.s);
            }
        }
        emit(" }\n");
        return 1;
    }
    case M(INC): case M(DEC): case M(NEG): case M(NOT): {
        const int size = d->size;
        emit("{ ");
        Str da;
        int stack = 0;
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        const char *calc = m == M(INC) ? "a + 1" : m == M(DEC) ? "a - 1" : m == M(NEG) ? "0 - a" : "~a";
        emit("const uint64_t v = (%s) & 0x%" PRIx64 "ull; ", calc, mask(size));
        if (m == M(INC) || m == M(DEC)) emit("fcf = " COND "; fk = %s; fa = a; fb = 1; fr = v; fz = %d; ", 2, m == M(INC) ? "FK_INC" : "FK_DEC", size);
        if (m == M(NEG)) emit("fk = FK_SUB; fa = 0; fb = a; fr = v; fz = %d; ", size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    case M(PUSH):
        if (d->size == 2) return 0;
        if (d->type == OP_REG && d->kind != RK_GPR) return 0;
        if (!read_op(off, in, d, 8, &b)) return 0;
        if (d->type == OP_IMM) b = str("0x%" PRIx64 "ull", (uint64_t)d->disp);
        emit("{ const uint64_t v = %s; r[RSP] -= 8; rc_sts(r[RSP], 8, v); }\n", b.s);
        return 1;
    case M(POP):
        if (!gpr(d) || d->size != 8) return 0;
        emit("{ const uint64_t v = rc_lds(r[RSP], 8); r[RSP] += 8; r[%d] = v; }\n", d->reg);
        return 1;
    case M(LEAVE):
        emit("r[RSP] = r[RBP]; r[RBP] = rc_lds(r[RSP], 8); r[RSP] += 8;\n");
        return 1;
    case M(JMP):
        if (d->type == OP_IMM) {
            jump(f, off + (uint64_t)d->disp);
            emit("\n");
            return 1;
        }
        if (!read_op(off, in, d, 8, &b)) return 0;
        emit("{ dn = %s; goto L_dispatch; }\n", b.s);
        ++stats[1];
        return 1;
    case M(CALL): {
        if (d->type == OP_IMM) {
            /* A recompiled callee directly, when the program filled its slot (the game: the ones it
             * runs; the replay leaves them empty and answers each call from the record). */
            const uint64_t t = off + (uint64_t)d->disp;
            if (is_recompiled(t)) {
                emit("{ extern RcFn d_%" PRIx64 "; " SPILL " if (d_%" PRIx64 ") { c->r[RSP] -= 8; rc_st(c->r[RSP], 8, B + 0x%" PRIx64
                     "ull); c->rip = B + 0x%" PRIx64 "ull; d_%" PRIx64 "(c); } else rc->call(c, B + 0x%" PRIx64 "ull, B + 0x%" PRIx64
                     "ull); " RELOAD " fk = FK_CPU; }\n", t, t, next, t, t, t, next);
                return 1;
            }
            b = str("B + 0x%" PRIx64 "ull", t);
        } else if (!read_op(off, in, d, 8, &b)) {
            return 0;
        }
        emit("{ const uint64_t t = %s; " SPILL " rc->call(c, t, B + 0x%" PRIx64 "ull); " RELOAD " fk = FK_CPU; }\n", b.s, next);
        return 1;
    }
    case M(RET): {
        const uint64_t pop = 8 + (in->count && d->type == OP_IMM ? (uint64_t)d->disp : 0);
        emit("{ const uint64_t t = rc_lds(r[RSP], 8); r[RSP] += %" PRIu64 "; " SPILL " c->rip = t; return; }\n", pop);
        return 1;
    }
    default:
        return translate_shift_mul(off, in) || translate_extra(off, in) || translate_vector(off, in);
    }
}


/* ---- vector instructions (as src/cpu/interp_vec.c does them) ---- */

static int vreg(const BbOp *op) { return op->type == OP_REG && op->kind == RK_VEC; }
static int vwidth(const BbInsn *in) { return in->vex && in->vl ? in->vl : 16; }

/* A vector operand's value (a BbVec expression). */
static int vread(uint64_t off, const BbInsn *in, const BbOp *op, Str *v) {
    if (vreg(op)) { *v = str("c->v[%d]", op->reg); return 1; }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *v = str("rc_vld(%s, %d)", a.s, op->size);
        return 1;
    }
    if (gpr(op)) {
        Str g;
        if (!read_op(off, in, op, 8, &g)) return 0;
        *v = str("rc_vgpr(%s)", g.s);
        return 1;
    }
    return 0;
}

/* A statement writing BbVec `v` (`bytes` of it to a register). */
static int vwrite(uint64_t off, const BbInsn *in, const BbOp *op, const char *v, int bytes, Str *s) {
    if (vreg(op)) { *s = str("rc_vset(&c->v[%d], %s, %d, %d);", op->reg, v, bytes, in->vex ? 1 : 0); return 1; }
    if (op->type == OP_MEM) {
        Str a;
        int stack;
        if (!address(off, in, op, &a, &stack)) return 0;
        *s = str("rc_vst(%s, %d, %s);", a.s, op->size, v);
        return 1;
    }
    return 0;
}

typedef struct { const BbOp *dst, *s1, *s2, *imm; } VOps;
static VOps vops3(const BbInsn *in) {
    VOps o = {&in->op[0], NULL, NULL, NULL};
    if (in->vex && in->count >= 3 && in->op[2].type != OP_IMM) {
        o.s1 = &in->op[1]; o.s2 = &in->op[2];
        if (in->count >= 4 && in->op[3].type == OP_IMM) o.imm = &in->op[3];
    } else {
        o.s1 = &in->op[0]; o.s2 = &in->op[1];
        if (in->count >= 3 && in->op[2].type == OP_IMM) o.imm = &in->op[2];
    }
    return o;
}
static VOps vops1(const BbInsn *in) {
    VOps o = {&in->op[0], NULL, NULL, NULL};
    int last = in->count - 1;
    if (in->op[last].type == OP_IMM) { o.imm = &in->op[last]; --last; }
    o.s2 = &in->op[last];
    o.s1 = &in->op[0];
    return o;
}

/* Two sources, a lane loop, the destination: `body` sets rr from a and b (i: the lane). */
static int vbinary(uint64_t off, const BbInsn *in, int lanes, int bytes, int zero, const char *body) {
    const VOps o = vops3(in);
    Str a, b, w;
    if (!vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", bytes, &w)) return 0;
    emit("{ const BbVec a = %s, b = %s; BbVec rr = a; %s for (int i = 0; i < %d; ++i) { %s } %s }\n", a.s, b.s,
         zero ? "memset(&rr, 0, sizeof(rr));" : "", lanes, body, w.s);
    return 1;
}

static int translate_vector(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic, w = vwidth(in);
    Str a, b, s;
    switch (m) {
    case M(MOVAPS): case M(MOVUPS): case M(MOVAPD): case M(MOVUPD): case M(MOVDQA): case M(MOVDQU):
    case M(VMOVAPS): case M(VMOVUPS): case M(VMOVAPD): case M(VMOVUPD): case M(VMOVDQA): case M(VMOVDQU):
    case M(LDDQU): case M(VLDDQU):
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], a.s, in->op[0].size, &s)) return 0;
        emit("%s\n", s.s);
        return 1;
    case M(MOVSS): case M(VMOVSS): case M(MOVSD): case M(VMOVSD): {
        if (m == M(MOVSD) && !(in->count && vreg(&in->op[0])) && !(in->count >= 2 && vreg(&in->op[1]))) return 0;
        const int bytes = (m == M(MOVSS) || m == M(VMOVSS)) ? 4 : 8;
        const BbOp *d = &in->op[0];
        if (d->type == OP_MEM) {
            Str ad;
            int stack;
            if (!vread(off, in, &in->op[in->count - 1], &a) || !address(off, in, d, &ad, &stack)) return 0;
            emit("rc_vst(%s, %d, %s);\n", ad.s, bytes, a.s);
            return 1;
        }
        if (!vreg(d)) return 0;
        if (in->count == 3) {
            if (!vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b)) return 0;
            emit("{ BbVec rr = %s; const BbVec b = %s; memcpy(rr.b, b.b, %d); rc_vset(&c->v[%d], rr, 16, %d); }\n", a.s, b.s,
                 bytes, d->reg, in->vex ? 1 : 0);
            return 1;
        }
        const BbOp *src = &in->op[1];
        if (src->type == OP_MEM) {
            Str ad;
            int stack;
            if (!address(off, in, src, &ad, &stack)) return 0;
            if (in->vex) emit("rc_vset(&c->v[%d], rc_vld(%s, %d), 16, 1);\n", d->reg, ad.s, bytes);
            else emit("{ const BbVec rr = rc_vld(%s, %d); memcpy(c->v[%d].b, rr.b, 16); }\n", ad.s, bytes, d->reg);
            return 1;
        }
        if (!vreg(src)) return 0;
        emit("{ BbVec rr = c->v[%d]; memcpy(rr.b, c->v[%d].b, %d); rc_vset(&c->v[%d], rr, 16, %d); }\n", d->reg,
             src->reg, bytes, d->reg, in->vex ? 1 : 0);
        return 1;
    }
    case M(ADDPS): case M(VADDPS): case M(SUBPS): case M(VSUBPS): case M(MULPS): case M(VMULPS):
    case M(DIVPS): case M(VDIVPS): case M(MINPS): case M(VMINPS): case M(MAXPS): case M(VMAXPS):
    case M(ADDSS): case M(VADDSS): case M(SUBSS): case M(VSUBSS): case M(MULSS): case M(VMULSS):
    case M(DIVSS): case M(VDIVSS): case M(MINSS): case M(VMINSS): case M(MAXSS): case M(VMAXSS):
    case M(ADDPD): case M(VADDPD): case M(SUBPD): case M(VSUBPD): case M(MULPD): case M(VMULPD):
    case M(DIVPD): case M(VDIVPD): case M(ADDSD): case M(VADDSD): case M(SUBSD): case M(VSUBSD):
    case M(MULSD): case M(VMULSD): case M(DIVSD): case M(VDIVSD): {
        const char *name = ZydisMnemonicGetString((ZydisMnemonic)m);
        const char *base = name[0] == 'v' ? name + 1 : name;
        const int dbl = base[strlen(base) - 1] == 'd', scalar = base[strlen(base) - 2] == 's';
        const char *f = dbl ? "fd" : "f", *t = dbl ? "double" : "float";
        const char *op = !strncmp(base, "add", 3) ? "x + y" : !strncmp(base, "sub", 3) ? "x - y"
                       : !strncmp(base, "mul", 3) ? "x * y" : !strncmp(base, "div", 3) ? "x / y"
                       : !strncmp(base, "min", 3) ? "x < y ? x : y" : "x > y ? x : y";
        const Str body = str("const %s x = a.%s[i], y = b.%s[i]; rr.%s[i] = %s;", t, f, f, f, op);
        return vbinary(off, in, scalar ? 1 : w / (dbl ? 8 : 4), scalar ? 16 : w, 0, body.s);
    }
    case M(ANDPS): case M(VANDPS): case M(ANDPD): case M(VANDPD): case M(PAND): case M(VPAND):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] & b.q[i];");
    case M(ANDNPS): case M(VANDNPS): case M(ANDNPD): case M(VANDNPD): case M(PANDN): case M(VPANDN):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = ~a.q[i] & b.q[i];");
    case M(ORPS): case M(VORPS): case M(ORPD): case M(VORPD): case M(POR): case M(VPOR):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] | b.q[i];");
    case M(XORPS): case M(VXORPS): case M(XORPD): case M(VXORPD): case M(PXOR): case M(VPXOR):
        return vbinary(off, in, 4, w, 0, "rr.q[i] = a.q[i] ^ b.q[i];");
    case M(PADDD): case M(VPADDD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] + b.d[i];");
    case M(PSUBD): case M(VPSUBD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] - b.d[i];");
    case M(PADDQ): case M(VPADDQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] + b.q[i];");
    case M(PSUBQ): case M(VPSUBQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] - b.q[i];");
    case M(PCMPEQB): case M(VPCMPEQB): return vbinary(off, in, w, w, 1, "rr.b[i] = a.b[i] == b.b[i] ? 0xff : 0;");
    case M(PCMPEQD): case M(VPCMPEQD): return vbinary(off, in, w / 4, w, 1, "rr.d[i] = a.d[i] == b.d[i] ? ~0u : 0;");
    case M(PCMPEQQ): case M(VPCMPEQQ): return vbinary(off, in, w / 8, w, 1, "rr.q[i] = a.q[i] == b.q[i] ? ~0ull : 0;");
    case M(HADDPS): case M(VHADDPS): case M(HSUBPS): case M(VHSUBPS): {
        const char *op = (m == M(HADDPS) || m == M(VHADDPS)) ? "+" : "-";
        const Str body = str("const float *x = a.f + i * 4, *y = b.f + i * 4; rr.f[i * 4] = x[0] %s x[1]; "
                             "rr.f[i * 4 + 1] = x[2] %s x[3]; rr.f[i * 4 + 2] = y[0] %s y[1]; rr.f[i * 4 + 3] = y[2] %s y[3];",
                             op, op, op, op);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(CMPPS): case M(VCMPPS): case M(CMPPD): case M(VCMPPD): case M(CMPSS): case M(VCMPSS): case M(CMPSD): case M(VCMPSD): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int predicate = (int)o.imm->disp & 31;
        const int dbl = m == M(CMPPD) || m == M(VCMPPD) || m == M(CMPSD) || m == M(VCMPSD);
        const int scalar = m == M(CMPSS) || m == M(VCMPSS) || m == M(CMPSD) || m == M(VCMPSD);
        const Str body = dbl ? str("rr.q[i] = rc_fcompare(a.fd[i], b.fd[i], %d) ? ~0ull : 0;", predicate)
                             : str("rr.d[i] = rc_fcompare(a.f[i], b.f[i], %d) ? ~0u : 0;", predicate);
        return vbinary(off, in, scalar ? 1 : w / (dbl ? 8 : 4), scalar ? 16 : w, 0, body.s);
    }
    case M(COMISS): case M(VCOMISS): case M(UCOMISS): case M(VUCOMISS): case M(COMISD): case M(VCOMISD):
    case M(UCOMISD): case M(VUCOMISD): {
        const int dbl = m == M(COMISD) || m == M(VCOMISD) || m == M(UCOMISD) || m == M(VUCOMISD);
        if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
        emit("{ const BbVec a = %s, b = %s; c->flags = rc_comis(c->flags, a.%s[0], b.%s[0]); fk = FK_CPU; }\n", a.s, b.s,
             dbl ? "fd" : "f", dbl ? "fd" : "f");
        return 1;
    }
    case M(SHUFPS): case M(VSHUFPS): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int imm = (int)o.imm->disp;
        const Str body = str("rr.d[i * 4] = a.d[i * 4 + %d]; rr.d[i * 4 + 1] = a.d[i * 4 + %d]; rr.d[i * 4 + 2] = b.d[i * 4 + %d]; "
                             "rr.d[i * 4 + 3] = b.d[i * 4 + %d];", imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(PSHUFD): case M(VPSHUFD): case M(VPERMILPS): {
        if (m == M(VPERMILPS) && in->op[2].type != OP_IMM) return 0;
        const VOps o = vops1(in);
        if (!o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp;
        emit("{ const BbVec a = %s; BbVec rr = a; for (int h = 0; h < %d; ++h) { rr.d[h * 4] = a.d[h * 4 + %d]; "
             "rr.d[h * 4 + 1] = a.d[h * 4 + %d]; rr.d[h * 4 + 2] = a.d[h * 4 + %d]; rr.d[h * 4 + 3] = a.d[h * 4 + %d]; } %s }\n",
             a.s, w / 16, imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3, s.s);
        return 1;
    }
    case M(MOVMSKPS): case M(VMOVMSKPS): {
        if (!vread(off, in, &in->op[1], &a)) return 0;
        Str wr;
        if (!write_op(off, in, &in->op[0], "mask", &wr)) return 0;
        emit("{ const BbVec a = %s; uint64_t mask = 0; for (int i = 0; i < %d; ++i) mask |= (uint64_t)(a.d[i] >> 31) << i; %s }\n",
             a.s, w / 4, wr.s);
        return 1;
    }
    case M(MOVHLPS): case M(VMOVHLPS): case M(MOVLHPS): case M(VMOVLHPS): {
        const VOps o = vops3(in);
        if (!vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        emit("{ BbVec rr = %s; const BbVec b = %s; %s %s }\n", a.s, b.s,
             (m == M(MOVHLPS) || m == M(VMOVHLPS)) ? "rr.q[0] = b.q[1];" : "rr.q[1] = b.q[0];", s.s);
        return 1;
    }
    case M(MOVD): case M(VMOVD): case M(MOVQ): case M(VMOVQ): {
        const BbOp *d = &in->op[0], *src = &in->op[1];
        const int bytes = (m == M(MOVD) || m == M(VMOVD)) ? 4 : 8;
        if (!vread(off, in, src, &a)) return 0;
        if (vreg(d)) {
            emit("{ BbVec rr; memset(&rr, 0, sizeof(rr)); const BbVec s = %s; memcpy(rr.b, s.b, %d); memcpy(c->v[%d].b, rr.b, 16);%s }\n",
                 a.s, bytes, d->reg, in->vex ? str(" memset(c->v[%d].b + 16, 0, 16);", d->reg).s : "");
            return 1;
        }
        if (d->type == OP_MEM) {
            Str ad;
            int stack;
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("rc_vst(%s, %d, %s);\n", ad.s, bytes, a.s);
            return 1;
        }
        Str wr;
        if (!write_op(off, in, d, bytes == 4 ? "s.d[0]" : "s.q[0]", &wr)) return 0;
        emit("{ const BbVec s = %s; %s }\n", a.s, wr.s);
        return 1;
    }
    case M(DPPS): case M(VDPPS): { /* the hardware's pairwise order: (p0 + p1) + (p2 + p3) */
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        const Str body = str("float p[4]; for (int k = 0; k < 4; ++k) p[k] = (%d & (0x10 << k)) ? a.f[i * 4 + k] * b.f[i * 4 + k] : 0.0f; "
                             "const float sum = (p[0] + p[1]) + (p[2] + p[3]); "
                             "for (int k = 0; k < 4; ++k) rr.f[i * 4 + k] = (%d & (1 << k)) ? sum : 0.0f;", imm, imm);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    case M(INSERTPS): case M(VINSERTPS): {
        const VOps o = vops3(in);
        if (!o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        emit("{ BbVec rr = %s; const BbVec b = %s; rr.d[%d] = b.d[%d];", a.s, b.s, (imm >> 4) & 3,
             o.s2->type == OP_MEM ? 0 : (imm >> 6) & 3);
        for (int i = 0; i < 4; ++i) if (imm & (1 << i)) emit(" rr.d[%d] = 0;", i);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(BLENDVPS): case M(VBLENDVPS): case M(BLENDVPD): case M(VBLENDVPD): case M(PBLENDVB): case M(VPBLENDVB): {
        Str mk;
        if (in->vex) {
            if (in->count < 4 || !vread(off, in, &in->op[1], &a) || !vread(off, in, &in->op[2], &b) ||
                !vread(off, in, &in->op[3], &mk)) return 0;
        } else {
            if (!vread(off, in, &in->op[0], &a) || !vread(off, in, &in->op[1], &b)) return 0;
            mk = str("c->v[0]");
        }
        if (!vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const int es = (m == M(BLENDVPS) || m == M(VBLENDVPS)) ? 4 : (m == M(BLENDVPD) || m == M(VBLENDVPD)) ? 8 : 1;
        emit("{ BbVec rr = %s; const BbVec b = %s, mk = %s; for (int i = 0; i < %d; ++i) if (mk.b[i * %d + %d] & 0x80) "
             "memcpy(rr.b + i * %d, b.b + i * %d, %d); %s }\n", a.s, b.s, mk.s, w / es, es, es - 1, es, es, es, s.s);
        return 1;
    }
    case M(BLENDPS): case M(VBLENDPS): case M(BLENDPD): case M(VBLENDPD): case M(PBLENDW): case M(VPBLENDW): case M(VPBLENDD): {
        const VOps o = vops3(in);
        if (!o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const int imm = (int)o.imm->disp & 0xff;
        const int es = (m == M(BLENDPS) || m == M(VBLENDPS) || m == M(VPBLENDD)) ? 4 : (m == M(BLENDPD) || m == M(VBLENDPD)) ? 8 : 2;
        emit("{ BbVec rr = %s; const BbVec b = %s;", a.s, b.s);
        for (int i = 0; i < w / es; ++i)
            if (imm & (1 << (es == 2 ? (i & 7) : i))) emit(" memcpy(rr.b + %d, b.b + %d, %d);", i * es, i * es, es);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(SQRTPS): case M(VSQRTPS): case M(RSQRTPS): case M(VRSQRTPS): case M(RCPPS): case M(VRCPPS): case M(SQRTPD): case M(VSQRTPD): {
        const VOps o = vops1(in);
        if (o.imm || !vread(off, in, o.s2, &a) || !vwrite(off, in, o.dst, "rr", w, &s)) return 0;
        const char *body = (m == M(SQRTPD) || m == M(VSQRTPD)) ? "for (int i = 0; i < %d; ++i) rr.fd[i] = __builtin_sqrt(x.fd[i]);"
                         : (m == M(SQRTPS) || m == M(VSQRTPS)) ? "for (int i = 0; i < %d; ++i) rr.f[i] = __builtin_sqrtf(x.f[i]);"
                         : (m == M(RCPPS) || m == M(VRCPPS)) ? "for (int i = 0; i < %d; ++i) rr.f[i] = 1.0f / x.f[i];"
                         : "for (int i = 0; i < %d; ++i) rr.f[i] = 1.0f / __builtin_sqrtf(x.f[i]);";
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); ", a.s);
        emit(body, (m == M(SQRTPD) || m == M(VSQRTPD)) ? w / 8 : w / 4);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(SQRTSS): case M(VSQRTSS): case M(RSQRTSS): case M(VRSQRTSS): case M(RCPSS): case M(VRCPSS): case M(SQRTSD): case M(VSQRTSD): {
        const VOps o = vops3(in);
        if (o.imm || !vread(off, in, o.s1, &a) || !vread(off, in, o.s2, &b) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const char *calc = (m == M(SQRTSD) || m == M(VSQRTSD)) ? "rr.fd[0] = __builtin_sqrt(x.fd[0]);"
                         : (m == M(SQRTSS) || m == M(VSQRTSS)) ? "rr.f[0] = __builtin_sqrtf(x.f[0]);"
                         : (m == M(RCPSS) || m == M(VRCPSS)) ? "rr.f[0] = 1.0f / x.f[0];" : "rr.f[0] = 1.0f / __builtin_sqrtf(x.f[0]);";
        emit("{ BbVec rr = %s; const BbVec x = %s; %s %s }\n", a.s, b.s, calc, s.s);
        return 1;
    }
    case M(CVTSI2SS): case M(VCVTSI2SS): case M(CVTSI2SD): case M(VCVTSI2SD): {
        const VOps o = vops3(in);
        Str g;
        if (o.imm || (o.s2->size != 4 && o.s2->size != 8) || !vread(off, in, o.s1, &a) || !read_op(off, in, o.s2, o.s2->size, &g) ||
            !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        emit("{ BbVec rr = %s; const int64_t x = (int64_t)(%s)(%s); %s %s }\n", a.s, o.s2->size == 8 ? "int64_t" : "int32_t", g.s,
             (m == M(CVTSI2SS) || m == M(VCVTSI2SS)) ? "rr.f[0] = (float)x;" : "rr.fd[0] = (double)x;", s.s);
        return 1;
    }
    case M(CVTTSS2SI): case M(VCVTTSS2SI): case M(CVTSS2SI): case M(VCVTSS2SI): case M(CVTTSD2SI): case M(VCVTTSD2SI):
    case M(CVTSD2SI): case M(VCVTSD2SI): {
        const BbOp *d = &in->op[0];
        Str wr;
        if (!gpr(d) || (d->size != 4 && d->size != 8) || !vread(off, in, &in->op[1], &a)) return 0;
        const int truncate = m == M(CVTTSS2SI) || m == M(VCVTTSS2SI) || m == M(CVTTSD2SI) || m == M(VCVTTSD2SI);
        const int dbl = m == M(CVTTSD2SI) || m == M(VCVTTSD2SI) || m == M(CVTSD2SI) || m == M(VCVTSD2SI);
        if (!write_op(off, in, d, d->size == 8 ? str("(uint64_t)rc_f2i64(x, %d, c->mxcsr)", truncate).s
                                               : str("(uint32_t)rc_f2i32(x, %d, c->mxcsr)", truncate).s, &wr)) return 0;
        emit("{ const BbVec sv = %s; const double x = sv.%s[0]; %s }\n", a.s, dbl ? "fd" : "f", wr.s);
        return 1;
    }
    case M(CVTDQ2PS): case M(VCVTDQ2PS): case M(CVTPS2DQ): case M(VCVTPS2DQ): case M(CVTTPS2DQ): case M(VCVTTPS2DQ): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const char *calc = (m == M(CVTDQ2PS) || m == M(VCVTDQ2PS)) ? "rr.f[i] = (float)x.sd[i];"
                         : (m == M(CVTTPS2DQ) || m == M(VCVTTPS2DQ)) ? "rr.sd[i] = rc_f2i32(x.f[i], 1, c->mxcsr);"
                         : "rr.sd[i] = rc_f2i32(x.f[i], 0, c->mxcsr);";
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; ++i) %s %s }\n", a.s, w / 4, calc, s.s);
        return 1;
    }
    case M(VBROADCASTSS): case M(VBROADCASTSD): case M(VBROADCASTF128): case M(VPBROADCASTD): case M(VPBROADCASTQ):
    case M(VPBROADCASTB): case M(VPBROADCASTW): {
        if (!vread(off, in, &in->op[1], &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        const int es = (m == M(VBROADCASTSS) || m == M(VPBROADCASTD)) ? 4 : (m == M(VBROADCASTSD) || m == M(VPBROADCASTQ)) ? 8
                     : m == M(VPBROADCASTB) ? 1 : m == M(VPBROADCASTW) ? 2 : 16;
        emit("{ const BbVec x = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); for (int i = 0; i < %d; i += %d) memcpy(rr.b + i, x.b, %d); %s }\n",
             a.s, w, es, es, s.s);
        return 1;
    }
    case M(UNPCKLPS): case M(VUNPCKLPS): case M(UNPCKHPS): case M(VUNPCKHPS):
        return vbinary(off, in, w / 16, w, 1, (m == M(UNPCKLPS) || m == M(VUNPCKLPS))
            ? "rr.d[i * 4] = a.d[i * 4]; rr.d[i * 4 + 1] = b.d[i * 4]; rr.d[i * 4 + 2] = a.d[i * 4 + 1]; rr.d[i * 4 + 3] = b.d[i * 4 + 1];"
            : "rr.d[i * 4] = a.d[i * 4 + 2]; rr.d[i * 4 + 1] = b.d[i * 4 + 2]; rr.d[i * 4 + 2] = a.d[i * 4 + 3]; rr.d[i * 4 + 3] = b.d[i * 4 + 3];");
    case M(UNPCKLPD): case M(VUNPCKLPD): case M(UNPCKHPD): case M(VUNPCKHPD):
        return vbinary(off, in, w / 16, w, 1, (m == M(UNPCKLPD) || m == M(VUNPCKLPD))
            ? "rr.q[i * 2] = a.q[i * 2]; rr.q[i * 2 + 1] = b.q[i * 2];" : "rr.q[i * 2] = a.q[i * 2 + 1]; rr.q[i * 2 + 1] = b.q[i * 2 + 1];");
    case M(PSLLW): case M(VPSLLW): case M(PSLLD): case M(VPSLLD): case M(PSLLQ): case M(VPSLLQ): case M(PSRLW): case M(VPSRLW):
    case M(PSRLD): case M(VPSRLD): case M(PSRLQ): case M(VPSRLQ): case M(PSRAW): case M(VPSRAW): case M(PSRAD): case M(VPSRAD):
    case M(PSLLDQ): case M(VPSLLDQ): case M(PSRLDQ): case M(VPSRLDQ): {
        const BbOp *src = in->vex ? &in->op[1] : &in->op[0], *cnt = in->vex ? &in->op[2] : &in->op[1];
        Str n;
        if (!vread(off, in, src, &a) || !vwrite(off, in, &in->op[0], "rr", w, &s)) return 0;
        if (cnt->type == OP_IMM) n = str("%" PRIu64 "ull", (uint64_t)cnt->disp & 0xff);
        else if (!vread(off, in, cnt, &b)) return 0;
        else n = str("(%s).q[0]", b.s);
        const char *body;
        switch (m) {
        case M(PSLLW): case M(VPSLLW): body = "for (int i = 0; i < %d / 2; ++i) rr.w[i] = n > 15 ? 0 : (uint16_t)(x.w[i] << n);"; break;
        case M(PSLLD): case M(VPSLLD): body = "for (int i = 0; i < %d / 4; ++i) rr.d[i] = n > 31 ? 0 : x.d[i] << n;"; break;
        case M(PSLLQ): case M(VPSLLQ): body = "for (int i = 0; i < %d / 8; ++i) rr.q[i] = n > 63 ? 0 : x.q[i] << n;"; break;
        case M(PSRLW): case M(VPSRLW): body = "for (int i = 0; i < %d / 2; ++i) rr.w[i] = n > 15 ? 0 : (uint16_t)(x.w[i] >> n);"; break;
        case M(PSRLD): case M(VPSRLD): body = "for (int i = 0; i < %d / 4; ++i) rr.d[i] = n > 31 ? 0 : x.d[i] >> n;"; break;
        case M(PSRLQ): case M(VPSRLQ): body = "for (int i = 0; i < %d / 8; ++i) rr.q[i] = n > 63 ? 0 : x.q[i] >> n;"; break;
        case M(PSRAW): case M(VPSRAW): body = "for (int i = 0; i < %d / 2; ++i) rr.sw[i] = (int16_t)(x.sw[i] >> (n > 15 ? 15 : n));"; break;
        case M(PSRAD): case M(VPSRAD): body = "for (int i = 0; i < %d / 4; ++i) rr.sd[i] = x.sd[i] >> (n > 31 ? 31 : n);"; break;
        case M(PSLLDQ): case M(VPSLLDQ):
            body = "for (int h = 0; h < %d / 16; ++h) for (int i = 0; i < 16; ++i) rr.b[h * 16 + i] = (uint64_t)i >= n ? x.b[h * 16 + i - n] : 0;"; break;
        default:
            body = "for (int h = 0; h < %d / 16; ++h) for (int i = 0; i < 16; ++i) rr.b[h * 16 + i] = i + n < 16 ? x.b[h * 16 + i + n] : 0;"; break;
        }
        emit("{ const BbVec x = %s; const uint64_t n = %s; BbVec rr; memset(&rr, 0, sizeof(rr)); ", a.s, n.s);
        emit(body, w);
        emit(" %s }\n", s.s);
        return 1;
    }
    case M(EXTRACTPS): case M(VEXTRACTPS): case M(PEXTRD): case M(VPEXTRD): case M(PEXTRQ): case M(VPEXTRQ):
    case M(PEXTRW): case M(VPEXTRW): case M(PEXTRB): case M(VPEXTRB): {
        if (in->count < 3 || in->op[2].type != OP_IMM || !vread(off, in, &in->op[1], &a)) return 0;
        const int imm = (int)in->op[2].disp;
        const Str v = (m == M(PEXTRQ) || m == M(VPEXTRQ)) ? str("x.q[%d]", imm & 1) : (m == M(PEXTRW) || m == M(VPEXTRW)) ? str("x.w[%d]", imm & 7)
                    : (m == M(PEXTRB) || m == M(VPEXTRB)) ? str("x.b[%d]", imm & 15) : str("x.d[%d]", imm & 3);
        BbOp dst = in->op[0];
        if (dst.type == OP_REG && dst.size < 4) dst.size = 4;
        Str wr;
        if (!write_op(off, in, &dst, v.s, &wr)) return 0;
        emit("{ const BbVec x = %s; %s }\n", a.s, wr.s);
        return 1;
    }
    case M(PINSRD): case M(VPINSRD): case M(PINSRQ): case M(VPINSRQ): case M(PINSRW): case M(VPINSRW): case M(PINSRB): case M(VPINSRB): {
        const VOps o = vops3(in);
        Str g;
        if (!o.imm || !vread(off, in, o.s1, &a) || !read_op(off, in, o.s2, o.s2->size, &g) || !vwrite(off, in, o.dst, "rr", 16, &s)) return 0;
        const int imm = (int)o.imm->disp;
        const Str set = (m == M(PINSRQ) || m == M(VPINSRQ)) ? str("rr.q[%d] = v;", imm & 1)
                      : (m == M(PINSRW) || m == M(VPINSRW)) ? str("rr.w[%d] = (uint16_t)v;", imm & 7)
                      : (m == M(PINSRB) || m == M(VPINSRB)) ? str("rr.b[%d] = (uint8_t)v;", imm & 15) : str("rr.d[%d] = (uint32_t)v;", imm & 3);
        emit("{ BbVec rr = %s; const uint64_t v = %s; %s %s }\n", a.s, g.s, set.s, s.s);
        return 1;
    }
    case M(PALIGNR): case M(VPALIGNR): {
        const VOps o = vops3(in);
        if (!o.imm) return 0;
        const Str body = str("uint8_t cat[32]; memcpy(cat, b.b + i * 16, 16); memcpy(cat + 16, a.b + i * 16, 16); "
                             "for (int k = 0; k < 16; ++k) rr.b[i * 16 + k] = k + %d < 32 ? cat[k + %d] : 0;",
                             (int)o.imm->disp & 0xff, (int)o.imm->disp & 0xff);
        return vbinary(off, in, w / 16, w, 1, body.s);
    }
    default:
        return 0;
    }
}

/* Locked read-modify-writes (and xadd, cmpxchg): a compare-and-exchange loop, as the interpreter. */
static int translate_atomic(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0], *src = &in->op[1];
    if (d->type != OP_MEM) return 0;
    const int size = d->size;
    if (size != 1 && size != 2 && size != 4 && size != 8) return 0;
    Str ad, b, w;
    int stack;
    if (!address(off, in, d, &ad, &stack)) return 0;
    const uint64_t mk = mask(size);
    switch (m) {
    case M(ADD): case M(SUB): case M(AND): case M(OR): case M(XOR): {
        const char *op = m == M(ADD) ? "+" : m == M(SUB) ? "-" : m == M(AND) ? "&" : m == M(OR) ? "|" : "^";
        if (!read_op(off, in, src, size, &b)) return 0;
        emit("{ const uint64_t ea = %s; const uint64_t b = %s; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a %s b) & 0x%" PRIx64 "ull, &seen)); const uint64_t v = (a %s b) & 0x%" PRIx64 "ull; "
             "fk = %s; fa = a; fb = b; fr = v; fz = %d; }\n", ad.s, b.s, size, size, op, mk, op, mk,
             m == M(ADD) ? "FK_ADD" : m == M(SUB) ? "FK_SUB" : "FK_LOGIC", size);
        return 1;
    }
    case M(INC): case M(DEC): {
        const char *op = m == M(INC) ? "+" : "-";
        emit("{ const uint64_t ea = %s; fcf = " COND "; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a %s 1) & 0x%" PRIx64 "ull, &seen)); const uint64_t v = (a %s 1) & 0x%" PRIx64 "ull; "
             "fk = %s; fa = a; fb = 1; fr = v; fz = %d; }\n", ad.s, 2, size, size, op, mk, op, mk,
             m == M(INC) ? "FK_INC" : "FK_DEC", size);
        return 1;
    }
    case M(XADD): {
        if (!gpr(src) || !read_op(off, in, src, size, &b) || !write_op(off, in, src, "a", &w)) return 0;
        emit("{ const uint64_t ea = %s; const uint64_t b = %s; uint64_t a, seen; do a = rc_ld(ea, %d); "
             "while (!rc_cas(ea, %d, a, (a + b) & 0x%" PRIx64 "ull, &seen)); fk = FK_ADD; fa = a; fb = b; "
             "fr = (a + b) & 0x%" PRIx64 "ull; fz = %d; %s }\n", ad.s, b.s, size, size, mk, mk, size, w.s);
        return 1;
    }
    case M(CMPXCHG): {
        const BbOp rax = {.type = OP_REG, .kind = RK_GPR, .reg = RAX, .size = (uint8_t)size};
        Str e;
        if (!gpr(src) || !read_op(off, in, src, size, &b) || !read_op(off, in, &rax, size, &e) ||
            !write_op(off, in, &rax, "seen", &w))
            return 0;
        emit("{ const uint64_t ea = %s; const uint64_t e = (%s) & 0x%" PRIx64 "ull; uint64_t seen; "
             "const int ok = rc_cas(ea, %d, e, %s, &seen); fk = FK_SUB; fa = e; fb = seen; fr = (e - seen) & 0x%" PRIx64 "ull; "
             "fz = %d; if (!ok) { %s } }\n", ad.s, e.s, mk, size, b.s, mk, size, w.s);
        return 1;
    }
    default:
        return 0;
    }
}

/* Sign extensions of rax; adc/sbb; bt/bts/btr/btc; shifts by cl. */
static int translate_extra(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0], *src = &in->op[1];
    static const char *const sext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
    Str a, b, w, ad;
    int stack = 0;
    switch (m) {
    case M(CBW): emit("r[RAX] = (r[RAX] & ~0xffffull) | ((uint64_t)(int16_t)(int8_t)r[RAX] & 0xffff);\n"); return 1;
    case M(CWDE): emit("r[RAX] = (uint32_t)(int32_t)(int16_t)r[RAX];\n"); return 1;
    case M(CDQE): emit("r[RAX] = (uint64_t)(int64_t)(int32_t)r[RAX];\n"); return 1;
    case M(CWD): emit("r[RDX] = (r[RDX] & ~0xffffull) | (((int16_t)r[RAX] < 0) ? 0xffff : 0);\n"); return 1;
    case M(CDQ): emit("r[RDX] = ((int32_t)r[RAX] < 0) ? 0xffffffffu : 0;\n"); return 1;
    case M(CQO): emit("r[RDX] = ((int64_t)r[RAX] < 0) ? ~0ull : 0;\n"); return 1;
    case M(ADC): case M(SBB): {
        const int size = d->size;
        if (!(gpr(d) || d->type == OP_MEM) || !read_op(off, in, src, size, &b)) return 0;
        emit("{ const uint64_t cy = " COND "; ", 2);
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", ad.s, stack ? "rc_lds" : "rc_ld", size);
        } else {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        }
        const char *op = m == M(ADC) ? "+" : "-";
        emit("const uint64_t b = %s; const uint64_t v = (a %s b %s cy) & 0x%" PRIx64 "ull; "
             "c->flags = rc_carry_flags(rc_flags(fk, fa, fb, fr, fz, fcf, c->flags), %d, a, b, cy, v, %d); fk = FK_CPU; ",
             b.s, op, op, mask(size), m == M(SBB), size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    case M(BT): case M(BTS): case M(BTR): case M(BTC): {
        const int size = d->size, bits = size * 8;
        if (size < 2 || !read_op(off, in, src, size, &b)) return 0;
        const char *calc = m == M(BTS) ? "a | bm" : m == M(BTR) ? "a & ~bm" : m == M(BTC) ? "a ^ bm" : "a";
        emit("{ ");
        if (d->type == OP_MEM) { /* register bit offsets (reaching outside the operand): the interpreter */
            if (src->type != OP_IMM || !address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const unsigned bit = (unsigned)(%s) & %d; ", ad.s, b.s, bits - 1);
            emit("const uint64_t a = %s(ea, %d); ", stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; const unsigned bit = (unsigned)(%s) & %d; ", a.s, b.s, bits - 1);
        } else {
            return 0;
        }
        emit("const uint64_t bm = 1ull << bit; c->flags = (rc_flags(fk, fa, fb, fr, fz, fcf, c->flags) & ~(uint64_t)F_CF) | "
             "((a & bm) ? F_CF : 0); fk = FK_CPU; ");
        if (m != M(BT)) {
            emit("const uint64_t v = %s; ", calc);
            if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
            else {
                if (!write_op(off, in, d, "v", &w)) return 0;
                emit("%s", w.s);
            }
        }
        emit(" }\n");
        return 1;
    }
    case M(SHL): case M(SHR): case M(SAR): {
        if (in->count < 2 || src->type != OP_REG || src->reg != RCX || src->size != 1) return 0;
        const int size = d->size, bits = size * 8;
        if (!sext[size]) return 0;
        emit("{ const unsigned n = (unsigned)r[RCX] & %d; if (n) { ", size == 8 ? 63 : 31);
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &ad, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", ad.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        if (m == M(SHL)) emit("const uint64_t v = n >= %d ? 0 : (a << n) & 0x%" PRIx64 "ull; ", bits, mask(size));
        else if (m == M(SHR)) emit("const uint64_t v = n >= %d ? 0 : a >> n; ", bits);
        else emit("const uint64_t v = (uint64_t)((int64_t)(%s)a >> (n >= %d ? %d : n)) & 0x%" PRIx64 "ull; ", sext[size], bits,
                  bits - 1, mask(size));
        emit("c->flags = rc_shift_flags(rc_flags(fk, fa, fb, fr, fz, fcf, c->flags), %d, %s, n, v, %d); fk = FK_CPU; ",
             m == M(SHL) ? 0 : m == M(SHR) ? 1 : 2, m == M(SAR) ? str("(uint64_t)(int64_t)(%s)a", sext[size]).s : "a", size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" } }\n");
        return 1;
    }
    default:
        return 0;
    }
}

/* shl/shr/sar by a constant; imul with two or three operands. */
static int translate_shift_mul(uint64_t off, const BbInsn *in) {
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0];
    const int size = d->size;
    if (size != 4 && size != 8 && size != 2 && size != 1) return 0;
    static const char *const sext[9] = {0, "int8_t", "int16_t", 0, "int32_t", 0, 0, 0, "int64_t"};
    Str a, w;
    if (m == M(SHL) || m == M(SHR) || m == M(SAR)) {
        unsigned count = 1;
        if (in->count > 1) {
            if (in->op[1].type != OP_IMM) return 0; /* by cl */
            count = (unsigned)in->op[1].disp;
        }
        count &= size == 8 ? 63 : 31;
        if (!count) { emit("/* shift by 0 */\n"); return 1; }
        const int bits = size * 8;
        Str da;
        int stack = 0;
        emit("{ ");
        if (d->type == OP_MEM) {
            if (!address(off, in, d, &da, &stack)) return 0;
            emit("const uint64_t ea = %s; const uint64_t a = %s(ea, %d); ", da.s, stack ? "rc_lds" : "rc_ld", size);
        } else if (gpr(d)) {
            if (!read_op(off, in, d, size, &a)) return 0;
            emit("const uint64_t a = %s; ", a.s);
        } else {
            return 0;
        }
        if (m == M(SHL)) emit("const uint64_t v = %s; ", count >= (unsigned)bits ? "0" : str("(a << %u) & 0x%" PRIx64 "ull", count, mask(size)).s);
        else if (m == M(SHR)) emit("const uint64_t v = %s; ", count >= (unsigned)bits ? "0" : str("a >> %u", count).s);
        else emit("const uint64_t v = (uint64_t)((int64_t)(%s)a >> %u) & 0x%" PRIx64 "ull; ", sext[size],
                  count >= (unsigned)bits ? (unsigned)bits - 1 : count, mask(size));
        emit("c->flags = rc_shift_flags(rc_flags(fk, fa, fb, fr, fz, fcf, c->flags), %d, %s, %u, v, %d); fk = FK_CPU; ",
             m == M(SHL) ? 0 : m == M(SHR) ? 1 : 2, m == M(SAR) ? str("(uint64_t)(int64_t)(%s)a", sext[size]).s : "a", count, size);
        if (d->type == OP_MEM) emit("%s(ea, %d, v);", stack ? "rc_sts" : "rc_st", size);
        else {
            if (!write_op(off, in, d, "v", &w)) return 0;
            emit("%s", w.s);
        }
        emit(" }\n");
        return 1;
    }
    if (m == M(IMUL) && in->count >= 2 && gpr(d) && size >= 2) {
        Str x, y;
        if (!read_op(off, in, in->count == 3 ? &in->op[1] : d, size, &x)) return 0;
        if (in->count == 3) {
            if (in->op[2].type != OP_IMM) return 0;
            y = str("(int64_t)%" PRId64 "ll", in->op[2].disp);
        } else {
            Str t;
            if (!read_op(off, in, &in->op[1], size, &t)) return 0;
            y = str("(int64_t)(%s)(%s)", sext[size], t.s);
        }
        if (!write_op(off, in, d, "v", &w)) return 0;
        emit("{ const __int128 full = (__int128)(int64_t)(%s)(%s) * %s; const uint64_t v = (uint64_t)full & 0x%" PRIx64 "ull; "
             "const int ov = (__int128)(int64_t)(%s)v != full; "
             "c->flags = rc_szp((rc_flags(fk, fa, fb, fr, fz, fcf, c->flags) & ~(uint64_t)(F_CF | F_OF)) | (ov ? F_CF | F_OF : 0), v, %d); "
             "fk = FK_CPU; %s }\n", sext[size], x.s, y.s, mask(size), sext[size], size, w.s);
        return 1;
    }
    return 0;
}

/* The instructions of [start, start + size) the code reaches from the entry: fall-through, direct
 * jumps and branches inside the function (data between them, such as jump tables, is never decoded;
 * a place only an indirect jump reaches goes back to the translator). Sorted by offset. */
typedef struct { uint64_t off; BbInsn in; } Insn;
static int insn_order(const void *a, const void *b) {
    const uint64_t x = ((const Insn *)a)->off, y = ((const Insn *)b)->off;
    return x < y ? -1 : x > y;
}
static Insn *decode(Function fn, size_t *count) {
    Insn *list = NULL;
    size_t n = 0, capacity = 0;
    uint8_t *seen = calloc(fn.size + 16, 1);
    uint64_t *work = malloc((fn.size + 16) * sizeof(*work));
    size_t pending = 0;
    work[pending++] = fn.offset;
    while (pending) {
        uint64_t off = work[--pending];
        while (off >= fn.offset && off < fn.offset + fn.size && !seen[off - fn.offset]) {
            const BbBlock *block = bbcpu_block(base + off);
            if (!block->count) break;
            int ended = 0;
            for (uint32_t i = 0; i < block->count && !ended; ++i) {
                if (off >= fn.offset + fn.size || seen[off - fn.offset]) { ended = 1; break; }
                const BbInsn *in = &block->insn[i];
                seen[off - fn.offset] = 1;
                if (n == capacity) {
                    capacity = capacity ? capacity * 2 : 256;
                    list = realloc(list, capacity * sizeof(*list));
                }
                list[n++] = (Insn){off, *in};
                const int m = in->mnemonic, cc = cc_of(m);
                if (((cc >= 0 && cc < 16) || m == M(JMP) || m == M(JRCXZ) || m == M(JECXZ) || m == M(LOOP) ||
                     m == M(LOOPE) || m == M(LOOPNE)) && in->op[0].type == OP_IMM) {
                    const uint64_t t = off + (uint64_t)in->op[0].disp;
                    if (t >= fn.offset && t < fn.offset + fn.size && !seen[t - fn.offset]) work[pending++] = t;
                }
                /* No fall-through after these. */
                if (m == M(JMP) || m == M(RET) || m == M(INVALID) || m == M(UD2) || m == M(HLT)) ended = 2;
                off += in->length;
            }
            if (ended == 2) break;
        }
    }
    free(work);
    free(seen);
    qsort(list, n, sizeof(*list), insn_order);
    *count = n;
    return list;
}

/* Snippet mode (fuzzing, tests/fuzz_recomp.c): one instruction, then a return at its end. */
static int snippet_mode;
/* BB_RECOMP_TRACE=1: a trace point before each instruction (replay --lockstep). */
static int trace_points;
static uint64_t snippet_line;

static int by_offset(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void generate(Function fn, uint64_t *stats) {
    size_t count;
    Insn *list = decode(fn, &count);
    const Range f = {fn.offset, fn.offset + fn.size};
    /* Labels only where a direct jump goes (and the entry): every other address an indirect jump
     * or an interpreted branch reaches goes back to the translator (L_dispatch). */
    uint64_t *targets = malloc((count + 1) * sizeof(*targets));
    size_t target_count = 0;
    targets[target_count++] = f.start;
    for (size_t i = 0; i < count; ++i) {
        const BbInsn *in = &list[i].in;
        const int cc = cc_of(in->mnemonic);
        if (((cc >= 0 && cc < 16) || in->mnemonic == M(JMP)) && in->op[0].type == OP_IMM) {
            const uint64_t t = list[i].off + (uint64_t)in->op[0].disp;
            if (t >= f.start && t < f.end) targets[target_count++] = t;
        }
    }
    qsort(targets, target_count, sizeof(*targets), by_offset);
    emit("\n/* 0x%" PRIx64 ", %" PRIu64 " bytes, %zu instructions */\n", fn.offset, fn.size, count);
    if (snippet_mode) emit("static void s_%" PRIx64 "(BbCpu *c) {\n", snippet_line);
    else emit("__attribute__((visibility(\"hidden\"))) void f_%" PRIx64 "(BbCpu *c) {\n", fn.offset);
    emit("    const uint64_t B = rc->image_base;\n");
    emit("    uint64_t r[16];\n    " RELOAD "\n");
    emit("    int fk = FK_CPU, fz = 8, fcf = 0;\n    uint64_t fa = 0, fb = 0, fr = 0, dn = 0;\n");
    emit("    const uint64_t entry_rsp = r[RSP];\n");
    emit("    (void)fa; (void)fb; (void)fr; (void)fz; (void)fcf; (void)entry_rsp; (void)dn;\n");
    for (size_t i = 0; i < count; ++i) {
        const Insn *x = &list[i];
        if (bsearch(&x->off, targets, target_count, sizeof(*targets), by_offset)) emit("L_%" PRIx64 ": ", x->off);
        if (trace_points) emit("rc->trace(0x%" PRIx64 ", r, c); ", x->off);
        ++stats[2];
        const long mark = ftell(out);
        if (translate(f, x->off, &x->in, stats)) {
            ++stats[0];
            continue;
        }
        fseek(out, mark, SEEK_SET); /* what a translation wrote before it gave up */
        /* The interpreter: state spilled, flags in cpu->flags; an unexpected next rip dispatches. */
        emit("/* %s */ { " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; const uint64_t n = rc->step(c); " RELOAD
             " if (n != B + 0x%" PRIx64 "ull) { dn = n; goto L_dispatch; } }\n",
             ZydisMnemonicGetString((ZydisMnemonic)x->in.mnemonic), x->off, x->off + x->in.length);
    }
    if (snippet_mode) emit("    " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; return;\n", f.end);
    emit("    dn = B + 0x%" PRIx64 "ull; /* past the end */\n", f.end);
    emit("L_dispatch:\n    switch (dn - B) {\n");
    for (size_t i = 0; i < target_count; ++i)
        if (!i || targets[i] != targets[i - 1]) emit("    case 0x%" PRIx64 ": goto L_%" PRIx64 ";\n", targets[i], targets[i]);
    emit("    default: break;\n    }\n");
    emit("    " SPILL " " FLAGS_OUT "\n    c->rip = dn;\n");
    emit("    if ((dn - B >= 0x%" PRIx64 "ull && dn - B < 0x%" PRIx64 "ull) || r[RSP] != entry_rsp) rc->bail(c, entry_rsp); /* the translator */\n", f.start, f.end);
    emit("    else rc->tail(c); /* out of the function, the stack as at its entry: a tail call */\n}\n");
    free(targets);
    free(list);
}

/* --snippets ENCODINGS OUT.c: each instruction (hex bytes per line) at CODE_AT, alone; the ones
 * translated natively get a function s_<line number>, in a table (offset: the line number). */
#define SNIPPET_AT UINT64_C(0x30000000000)
static int snippets(const char *encodings, const char *path) {
    uint8_t *code = mmap((void *)SNIPPET_AT, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
    base = SNIPPET_AT;
    bbcpu_add_guest_code(SNIPPET_AT, 0x4000);
    static BbCpu warm;
    bbcpu_run(&warm, 0);
    snippet_mode = 1;
    FILE *in = fopen(encodings, "r");
    out = fopen(path, "w");
    if (!in || !out) { perror(encodings); return 1; }
    emit("/* Written by tools/recomp/bbrecomp.c --snippets (tests/fuzz_recomp.c). */\n");
    emit("#include \"recomp/rc.h\"\n\nstatic const RcApi *rc;\n");
    char line[128];
    uint64_t stats[3] = {0, 0, 0}, number = 0;
    uint64_t *lines = NULL;
    size_t count = 0, capacity = 0;
    while (fgets(line, sizeof(line), in)) {
        ++number;
        uint8_t bytes[16];
        int len = 0;
        for (const char *p = line; p[0] && p[1] && p[0] != '\n' && len < 15; p += 2) {
            unsigned v;
            if (sscanf(p, "%2x", &v) != 1) break;
            bytes[len++] = (uint8_t)v;
        }
        if (!len) continue;
        memset(code, 0x90, 64);
        memcpy(code, bytes, (size_t)len);
        bbcpu_invalidate(SNIPPET_AT, 64);
        const BbBlock *block = bbcpu_block(SNIPPET_AT);
        if (!block->count || block->insn[0].length != len || block->insn[0].branch) continue;
        /* Natively translated only: a fallback is the interpreter itself. */
        const long mark = ftell(out);
        const uint64_t before = stats[0];
        snippet_line = number;
        generate((Function){0, (uint64_t)len}, stats);
        if (stats[0] == before) { fseek(out, mark, SEEK_SET); continue; }
        if (count == capacity) { capacity = capacity ? capacity * 2 : 4096; lines = realloc(lines, capacity * 8); }
        lines[count++] = number;
    }
    emit("\nstatic const RcFunction table[] = {\n");
    for (size_t i = 0; i < count; ++i) emit("    {%" PRIu64 ", 0, 0, s_%" PRIx64 "},\n", lines[i], lines[i]);
    emit("};\n\nconst RcFunction *bb_recomp_init(const RcApi *api, size_t *count) {\n");
    emit("    rc = api;\n    *count = sizeof(table) / sizeof(table[0]);\n    return table;\n}\n");
    close_output();
    printf("%zu instructions translated natively (of %" PRIu64 " lines)\n", count, number);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--snippets")) return snippets(argv[2], argv[3]);
    if (argc < 5) {
        fprintf(stderr, "usage: %s ELF FUNCTIONS_TSV OUT.c OFFSET...\n", argv[0]);
        return 2;
    }
    map_image(argv[1]);
    static BbCpu warm;
    bbcpu_run(&warm, 0); /* the interpreter's tables */
    trace_points = getenv("BB_RECOMP_TRACE") && getenv("BB_RECOMP_TRACE")[0] == '1';
    functions = calloc((size_t)argc, sizeof(*functions));
    for (int i = 4; i < argc; ++i) {
        const uint64_t offset = strtoull(argv[i], NULL, 0);
        const uint64_t size = size_of(argv[2], offset);
        if (!size) { fprintf(stderr, "%#" PRIx64 ": not in %s\n", offset, argv[2]); return 1; }
        functions[function_count++] = (Function){offset, size};
    }
    /* Functions that call setjmp stay with the translator: the setjmp's return address would be
     * the runtime's (rc->call), and a longjmp cannot come back into a C function that went on.
     * From calls.tsv and imports.tsv next to FUNCTIONS_TSV (tools/recomp/scan.c, imports.py). */
    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", argv[2]);
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else dir[0] = 0;
        char path[1100], line[512];
        snprintf(path, sizeof(path), "%simports.tsv", dir);
        uint64_t jumps[8];
        int jump_count = 0;
        FILE *f = fopen(path, "r");
        while (f && fgets(line, sizeof(line), f)) {
            char name[256];
            unsigned long long stub;
            if (sscanf(line, "%llx\t%255s", &stub, name) == 2 &&
                (!strcmp(name, "setjmp") || !strcmp(name, "_setjmp") || !strcmp(name, "sigsetjmp")) && jump_count < 8)
                jumps[jump_count++] = stub;
        }
        if (f) fclose(f);
        else fprintf(stderr, "%s: none (functions that call setjmp are not known)\n", path);
        snprintf(path, sizeof(path), "%scalls.tsv", dir);
        f = jump_count ? fopen(path, "r") : NULL;
        size_t skipped = 0;
        while (f && fgets(line, sizeof(line), f)) {
            unsigned long long caller, callee;
            if (sscanf(line, "%llx\t%llx", &caller, &callee) != 2) continue;
            for (int j = 0; j < jump_count; ++j) {
                if (callee != jumps[j]) continue;
                for (size_t i = 0; i < function_count; ++i)
                    if (functions[i].offset == caller) { functions[i] = functions[--function_count]; ++skipped; break; }
            }
        }
        if (f) fclose(f);
        if (skipped) printf("%zu functions call setjmp: left to the translator\n", skipped);
    }
    /* By offset, once each. */
    qsort(functions, function_count, sizeof(*functions), by_offset); /* offset first in Function */
    size_t unique = 0;
    for (size_t i = 0; i < function_count; ++i)
        if (!unique || functions[i].offset != functions[unique - 1].offset) functions[unique++] = functions[i];
    function_count = unique;
    recompiled = malloc(function_count * sizeof(*recompiled));
    for (size_t i = 0; i < function_count; ++i) recompiled[i] = functions[i].offset;
    /* A file per WINDOW of the image (OUT_<window>.c, compiled in parallel; the table in OUT.c):
     * adding functions changes only the files of their windows, and recomp.sh compiles only files
     * that changed. */
    enum { WINDOW = 0x20000 };
    char path[1024];
    const size_t stem = strlen(argv[3]) - (strlen(argv[3]) > 2 && !strcmp(argv[3] + strlen(argv[3]) - 2, ".c") ? 2 : 0);
    uint64_t stats[3] = {0, 0, 0}; /* translated, indirect jumps, instructions */
    int files = 0;
    uint64_t window = UINT64_MAX;
    out = NULL;
    for (size_t i = 0; i < function_count; ++i) {
        if (functions[i].offset / WINDOW != window) {
            if (out) close_output();
            window = functions[i].offset / WINDOW;
            snprintf(path, sizeof(path), "%.*s_%05" PRIx64 ".c", (int)stem, argv[3], window);
            out = fopen(path, "w");
            if (!out) { perror(path); return 1; }
            ++files;
            emit("/* Written by tools/recomp/bbrecomp.c from the game's eboot.bin: not to be distributed. */\n");
            emit("#include \"recomp/rc.h\"\n\nextern const RcApi *rc;\n");
        }
        generate(functions[i], stats);
    }
    if (out) close_output();
    out = fopen(argv[3], "w");
    if (!out) { perror(argv[3]); return 1; }
    emit("/* Written by tools/recomp/bbrecomp.c from the game's eboot.bin: not to be distributed. */\n");
    emit("#include \"recomp/rc.h\"\n\n__attribute__((visibility(\"hidden\"))) const RcApi *rc;\n");
    for (size_t i = 0; i < function_count; ++i)
        emit("void f_%" PRIx64 "(BbCpu *c);\n__attribute__((visibility(\"hidden\"))) RcFn d_%" PRIx64 ";\n",
             functions[i].offset, functions[i].offset);
    emit("\nstatic const RcFunction table[] = {\n");
    for (size_t i = 0; i < function_count; ++i)
        emit("    {0x%" PRIx64 ", %" PRIu64 ", 0x%" PRIx64 "ull, f_%" PRIx64 ", &d_%" PRIx64 "},\n", functions[i].offset,
             functions[i].size, bbcpu_recomp_hash(base + functions[i].offset, functions[i].size), functions[i].offset,
             functions[i].offset);
    emit("};\n\n__attribute__((visibility(\"default\"))) const RcFunction *bb_recomp_init(const RcApi *api, size_t *count) {\n");
    emit("    rc = api;\n    *count = sizeof(table) / sizeof(table[0]);\n    return table;\n}\n");
    fclose(out);
    printf("%zu functions, %" PRIu64 " instructions: %" PRIu64 " in C (%.0f%%), the rest in the interpreter; %d files\n",
           function_count, stats[2], stats[0], stats[2] ? 100.0 * (double)stats[0] / (double)stats[2] : 0.0, files);
    return 0;
}
