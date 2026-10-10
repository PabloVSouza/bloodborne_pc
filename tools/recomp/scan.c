/* tools/recomp/scan.c ELF OUTDIR: the game's function table for the decompilation (docs/RECOMPILATION.md).
 * Functions come from the unwind tables (PT_GNU_EH_FRAME: every function the compiler emitted
 * unwind information for, with its exact start and size). Each one is decoded with Zydis:
 *   OUTDIR/functions.tsv  address, size, instructions, undecodable bytes
 *   OUTDIR/calls.tsv      caller, callee (direct calls, and jumps to another function's start)
 *   OUTDIR/slots.tsv      caller, slot (calls and jumps through a RIP-relative pointer: imports)
 *   OUTDIR/strings.tsv    function, string address, string (RIP-relative references to C strings)
 * Addresses are image offsets (the image is loaded at a base the runtime picks). */
#include <Zydis/Zydis.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ELF64 structures used (macOS has no <elf.h>). */
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_align; } Elf64_Phdr;
enum { PT_LOAD = 1, PT_GNU_EH_FRAME = 0x6474e550, PF_X = 1 };

static unsigned char *data;
static long data_size;
static Elf64_Phdr *loads[8];
static int nloads;

/* The file bytes at image address `va` with at least `len` bytes after them, or NULL. */
static const unsigned char *at(uint64_t va, uint64_t len) {
    for (int i = 0; i < nloads; ++i) {
        const Elf64_Phdr *p = loads[i];
        if (va >= p->p_vaddr && va + len <= p->p_vaddr + p->p_filesz)
            return data + p->p_offset + (va - p->p_vaddr);
    }
    return NULL;
}

typedef struct { uint64_t start, size; } Function;
static int by_start(const void *a, const void *b) {
    const Function *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* A C string at `va`: at least 4 printable bytes (UTF-8 allowed) and a NUL; its length, or 0. */
static size_t c_string(uint64_t va) {
    const unsigned char *s = at(va, 1);
    if (!s) return 0;
    size_t n = 0;
    while (at(va + n, 1) && s[n]) {
        if (s[n] < 0x20 && s[n] != '\t' && s[n] != '\n' && s[n] != '\r') return 0;
        if (++n > 4096) return 0;
    }
    return at(va + n, 1) && n >= 4 ? n : 0;
}

static void write_string(FILE *f, const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\t') fputs("\\t", f);
        else if (s[i] == '\n') fputs("\\n", f);
        else if (s[i] == '\r') fputs("\\r", f);
        else if (s[i] == '\\') fputs("\\\\", f);
        else fputc(s[i], f);
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s ELF OUTDIR\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END); data_size = ftell(in); fseek(in, 0, SEEK_SET);
    data = malloc(data_size);
    if (fread(data, 1, data_size, in) != (size_t)data_size) return 1;
    fclose(in);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    const Elf64_Phdr *eh_frame = NULL;
    for (int p = 0; p < eh->e_phnum; ++p) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(data + eh->e_phoff + p * eh->e_phentsize);
        if (ph->p_type == PT_LOAD && nloads < 8 && ph->p_offset + ph->p_filesz <= (uint64_t)data_size)
            loads[nloads++] = ph;
        if (ph->p_type == PT_GNU_EH_FRAME) eh_frame = ph;
    }
    if (!eh_frame) { fprintf(stderr, "no PT_GNU_EH_FRAME\n"); return 1; }

    /* .eh_frame_hdr: version, eh_frame_ptr_enc, fde_count_enc, table_enc, then the search table
     * of (initial location, FDE address) pairs, both datarel sdata4 (0x3b) from the header. */
    const unsigned char *hdr = data + eh_frame->p_offset;
    if (hdr[0] != 1 || hdr[2] != 0x03 || hdr[3] != 0x3b) {
        fprintf(stderr, "unexpected .eh_frame_hdr encodings %#x %#x %#x\n", hdr[1], hdr[2], hdr[3]);
        return 1;
    }
    uint32_t count;
    memcpy(&count, hdr + 8, 4);
    Function *functions = calloc(count, sizeof(Function));
    uint32_t nfunctions = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t loc, fde_rel;
        memcpy(&loc, hdr + 12 + 8 * i, 4);
        memcpy(&fde_rel, hdr + 16 + 8 * i, 4);
        /* FDE: length, CIE pointer, pc_begin (pcrel sdata4: augmentation "zR" with 0x1b), pc_range. */
        const uint64_t fde = eh_frame->p_vaddr + fde_rel;
        const unsigned char *f = at(fde, 16);
        if (!f) continue;
        uint32_t range;
        memcpy(&range, f + 12, 4);
        functions[nfunctions++] = (Function){eh_frame->p_vaddr + loc, range};
    }
    qsort(functions, nfunctions, sizeof(Function), by_start);

    char path[1024];
    FILE *out_functions, *out_calls, *out_slots, *out_strings;
#define OPEN(var, name)                                                                   \
    snprintf(path, sizeof(path), "%s/" name, argv[2]);                                    \
    if (!(var = fopen(path, "w"))) { perror(path); return 1; }
    OPEN(out_functions, "functions.tsv");
    OPEN(out_calls, "calls.tsv");
    OPEN(out_slots, "slots.tsv");
    OPEN(out_strings, "strings.tsv");
    fputs("address\tsize\tinstructions\tundecodable\n", out_functions);
    fputs("caller\tcallee\n", out_calls);
    fputs("caller\tslot\n", out_slots);
    fputs("function\taddress\tstring\n", out_strings);

    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint64_t total_instructions = 0, total_calls = 0, total_strings = 0;
    for (uint32_t i = 0; i < nfunctions; ++i) {
        const Function fn = functions[i];
        const unsigned char *code = at(fn.start, fn.size);
        if (!code) continue;
        uint64_t off = 0, instructions = 0, undecodable = 0;
        ZydisDecodedInstruction ins;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        while (off < fn.size) {
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + off, fn.size - off, &ins, ops))) {
                ++undecodable; ++off;
                continue;
            }
            ++instructions;
            const uint64_t next = fn.start + off + ins.length;
            const int is_call = ins.mnemonic == ZYDIS_MNEMONIC_CALL;
            const int is_jmp = ins.mnemonic == ZYDIS_MNEMONIC_JMP;
            if ((is_call || is_jmp) && ins.operand_count_visible) {
                const ZydisDecodedOperand *op = &ops[0];
                if (op->type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op->imm.is_relative) {
                    const uint64_t target = next + op->imm.value.s;
                    if (is_call || target < fn.start || target >= fn.start + fn.size) {
                        fprintf(out_calls, "%#llx\t%#llx\n", (unsigned long long)fn.start,
                                (unsigned long long)target);
                        ++total_calls;
                    }
                } else if (op->type == ZYDIS_OPERAND_TYPE_MEMORY && op->mem.base == ZYDIS_REGISTER_RIP &&
                           op->mem.index == ZYDIS_REGISTER_NONE) {
                    fprintf(out_slots, "%#llx\t%#llx\n", (unsigned long long)fn.start,
                            (unsigned long long)(next + op->mem.disp.value));
                }
            } else {
                for (int o = 0; o < ins.operand_count_visible; ++o) {
                    const ZydisDecodedOperand *op = &ops[o];
                    if (op->type != ZYDIS_OPERAND_TYPE_MEMORY || op->mem.base != ZYDIS_REGISTER_RIP ||
                        op->mem.index != ZYDIS_REGISTER_NONE)
                        continue;
                    const uint64_t target = next + op->mem.disp.value;
                    const size_t n = ins.mnemonic == ZYDIS_MNEMONIC_LEA ? c_string(target) : 0;
                    if (n) {
                        fprintf(out_strings, "%#llx\t%#llx\t", (unsigned long long)fn.start,
                                (unsigned long long)target);
                        write_string(out_strings, at(target, n), n);
                        fputc('\n', out_strings);
                        ++total_strings;
                    }
                }
            }
            off += ins.length;
        }
        total_instructions += instructions;
        fprintf(out_functions, "%#llx\t%llu\t%llu\t%llu\n", (unsigned long long)fn.start,
                (unsigned long long)fn.size, (unsigned long long)instructions,
                (unsigned long long)undecodable);
    }
    fclose(out_functions); fclose(out_calls); fclose(out_slots); fclose(out_strings);
    printf("%u functions, %llu instructions, %llu direct calls, %llu string references\n",
           nfunctions, (unsigned long long)total_instructions, (unsigned long long)total_calls,
           (unsigned long long)total_strings);
    return 0;
}
