/* tools/recomp/bbrecomp.c ELF FUNCTIONS_TSV OUT.c OFFSET...: the recompiler (docs/RECOMPILATION.md). Writes
 * one C function per game function (image offsets; sizes from tools/recomp/scan.c's functions.tsv)
 * against src/recomp/rc.h, and the library's entry point. Build: tools/recomp/recomp.sh.
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

#define SPILL "memcpy(c->r, r, sizeof(r));"
#define RELOAD "memcpy(r, c->r, sizeof(r));"
#define COND "rc_cond(%d, fk, fa, fb, fr, fz, fcf, c->flags)"
#define FLAGS_OUT "c->flags = rc_flags(fk, fa, fb, fr, fz, fcf, c->flags); fk = FK_CPU;"

typedef struct { char s[256]; } Str;
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

/* A jump to image offset `t`: a goto inside the function, else a tail call. */
static void jump(Range f, uint64_t t) {
    if (t >= f.start && t < f.end) emit("goto L_%" PRIx64 ";", t);
    else emit("{ " SPILL " c->rip = B + 0x%" PRIx64 "ull; rc->tail(c); return; }", t);
}

static int translate_vector(uint64_t off, const BbInsn *in);
static int translate_shift_mul(uint64_t off, const BbInsn *in);

/* One instruction natively; 0: it goes to the interpreter. */
static int translate(Range f, uint64_t off, const BbInsn *in, uint64_t *stats) {
    const uint64_t next = off + in->length;
    const int m = in->mnemonic;
    const BbOp *d = &in->op[0], *s = &in->op[1];
    Str a, b, w;
    if (in->lock || in->rep || in->repne) return 0;
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
            /* Every call through the runtime, recompiled callees included (it runs them directly):
             * the replay answers each call from the record. */
            b = str("B + 0x%" PRIx64 "ull", off + (uint64_t)d->disp);
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
        return translate_shift_mul(off, in) || translate_vector(off, in);
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

/* Decodes [start, start + size) linearly; returns the instructions (and their offsets). */
typedef struct { uint64_t off; BbInsn in; } Insn;
static Insn *decode(Function fn, size_t *count) {
    Insn *list = NULL;
    size_t n = 0, capacity = 0;
    for (uint64_t off = fn.offset; off < fn.offset + fn.size;) {
        const BbBlock *block = bbcpu_block(base + off);
        if (!block->count) break;
        for (uint32_t i = 0; i < block->count && off < fn.offset + fn.size; ++i) {
            if (n == capacity) {
                capacity = capacity ? capacity * 2 : 256;
                list = realloc(list, capacity * sizeof(*list));
            }
            list[n++] = (Insn){off, block->insn[i]};
            off += block->insn[i].length;
        }
    }
    *count = n;
    return list;
}

static void generate(Function fn, uint64_t *stats) {
    size_t count;
    Insn *list = decode(fn, &count);
    const Range f = {fn.offset, fn.offset + fn.size};
    emit("\n/* 0x%" PRIx64 ", %" PRIu64 " bytes, %zu instructions */\n", fn.offset, fn.size, count);
    emit("static void f_%" PRIx64 "(BbCpu *c) {\n", fn.offset);
    emit("    const uint64_t B = rc->image_base;\n");
    emit("    uint64_t r[16];\n    " RELOAD "\n");
    emit("    int fk = FK_CPU, fz = 8, fcf = 0;\n    uint64_t fa = 0, fb = 0, fr = 0, dn = 0;\n");
    emit("    const uint64_t entry_rsp = r[RSP];\n");
    emit("    (void)fa; (void)fb; (void)fr; (void)fz; (void)fcf; (void)entry_rsp; (void)dn;\n");
    for (size_t i = 0; i < count; ++i) {
        const Insn *x = &list[i];
        emit("L_%" PRIx64 ": ", x->off);
        ++stats[2];
        if (translate(f, x->off, &x->in, stats)) {
            ++stats[0];
            continue;
        }
        /* The interpreter: state spilled, flags in cpu->flags; an unexpected next rip dispatches. */
        emit("/* %s */ { " SPILL " " FLAGS_OUT " c->rip = B + 0x%" PRIx64 "ull; const uint64_t n = rc->step(c); " RELOAD
             " if (n != B + 0x%" PRIx64 "ull) { dn = n; goto L_dispatch; } }\n",
             ZydisMnemonicGetString((ZydisMnemonic)x->in.mnemonic), x->off, x->off + x->in.length);
    }
    emit("    dn = B + 0x%" PRIx64 "ull; /* past the end */\n", f.end);
    emit("L_dispatch:\n    switch (dn - B) {\n");
    for (size_t i = 0; i < count; ++i) emit("    case 0x%" PRIx64 ": goto L_%" PRIx64 ";\n", list[i].off, list[i].off);
    emit("    default: break;\n    }\n");
    emit("    " SPILL " " FLAGS_OUT "\n    c->rip = dn;\n");
    emit("    if (dn - B >= 0x%" PRIx64 "ull && dn - B < 0x%" PRIx64 "ull) rc->bail(c, entry_rsp); /* into the function: the translator */\n", f.start, f.end);
    emit("    else rc->tail(c); /* out of it: a tail call */\n}\n");
    free(list);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s ELF FUNCTIONS_TSV OUT.c OFFSET...\n", argv[0]);
        return 2;
    }
    map_image(argv[1]);
    static BbCpu warm;
    bbcpu_run(&warm, 0); /* the interpreter's tables */
    functions = calloc((size_t)argc, sizeof(*functions));
    for (int i = 4; i < argc; ++i) {
        const uint64_t offset = strtoull(argv[i], NULL, 0);
        const uint64_t size = size_of(argv[2], offset);
        if (!size) { fprintf(stderr, "%#" PRIx64 ": not in %s\n", offset, argv[2]); return 1; }
        functions[function_count++] = (Function){offset, size};
    }
    out = fopen(argv[3], "w");
    if (!out) { perror(argv[3]); return 1; }
    emit("/* Written by tools/recomp/bbrecomp.c from the game's eboot.bin: not to be distributed. */\n");
    emit("#include \"recomp/rc.h\"\n\nstatic const RcApi *rc;\n");
    for (size_t i = 0; i < function_count; ++i) emit("static void f_%" PRIx64 "(BbCpu *c);\n", functions[i].offset);
    uint64_t stats[3] = {0, 0, 0}; /* translated, indirect jumps, instructions */
    for (size_t i = 0; i < function_count; ++i) generate(functions[i], stats);
    emit("\nstatic const RcFunction table[] = {\n");
    for (size_t i = 0; i < function_count; ++i)
        emit("    {0x%" PRIx64 ", %" PRIu64 ", 0x%" PRIx64 "ull, f_%" PRIx64 "},\n", functions[i].offset,
             functions[i].size, bbcpu_recomp_hash(base + functions[i].offset, functions[i].size), functions[i].offset);
    emit("};\n\nconst RcFunction *bb_recomp_init(const RcApi *api, size_t *count) {\n");
    emit("    rc = api;\n    *count = sizeof(table) / sizeof(table[0]);\n    return table;\n}\n");
    fclose(out);
    printf("%zu functions, %" PRIu64 " instructions: %" PRIu64 " in C (%.0f%%), the rest in the interpreter\n",
           function_count, stats[2], stats[0], stats[2] ? 100.0 * (double)stats[0] / (double)stats[2] : 0.0);
    return 0;
}
