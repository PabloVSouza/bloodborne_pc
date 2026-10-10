/* bbcpu: per-thread guest CPU state and host -> guest calls. */
#include "cpu_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* The guest's stacks stay below 1 TiB like the PS4's (runtime_memory.c). */
void *runtime_low_map(size_t size, int prot);

enum { GUEST_STACK = 1 << 20, RED_ZONE = 128 };
/* Return addresses of host -> guest calls: never guest code, never a host function. */
#define SENTINEL(depth) (UINT64_C(0x0000700000000000) + (uint64_t)(depth) * 16)

static _Thread_local BbCpu *tls_cpu;
static _Thread_local uint64_t tls_thread_pointer;
static uint32_t tls_displacement;
static uint64_t cpus_created;

int bbcpu_enabled(void) {
#if defined(__aarch64__)
    return 1;
#else
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("BB_CPU");
        enabled = env && env[0] == 'i';
    }
    return enabled;
#endif
}

void bbcpu_set_tls_displacement(uint32_t displacement) { tls_displacement = displacement; }

static void apply_thread_pointer(BbCpu *cpu) {
    cpu->tcb = tls_thread_pointer;
    cpu->fs_base = tls_thread_pointer;
    /* The loader rewrote `mov rax, fs:[0]` into `mov rax, gs:[displacement]`. */
    cpu->gs_base = (uint64_t)(uintptr_t)&cpu->tcb - tls_displacement;
}

void bbcpu_set_thread_pointer(uintptr_t tcb) {
    tls_thread_pointer = tcb;
    if (tls_cpu) apply_thread_pointer(tls_cpu);
}

static BbCpu *current_cpu(void) {
    if (tls_cpu) return tls_cpu;
    BbCpu *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) abort();
    cpu->mxcsr = 0x1f80;
    cpu->fcw = 0x37f;
    cpu->flags = 2;
    apply_thread_pointer(cpu);
    __atomic_add_fetch(&cpus_created, 1, __ATOMIC_RELAXED);
    tls_cpu = cpu;
    return cpu;
}

uint64_t bbcpu_guest_rip(void) { return tls_cpu ? tls_cpu->rip : 0; }

static uint64_t run_call(BbCpu *cpu, uintptr_t fn, int count, const uint64_t *args, uint64_t rsp) {
    static const int arg_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    if (count > 6) {
        fputs("bbcpu: more than 6 arguments to a guest call\n", stderr);
        abort();
    }
    for (int i = 0; i < count; ++i) cpu->r[arg_regs[i]] = args[i];
    const uint64_t stop = SENTINEL(cpu->depth);
    rsp &= ~UINT64_C(15);
    rsp -= 8;
    bb_store(rsp, 8, stop); /* the return address: rsp % 16 == 8 at the callee's entry */
    cpu->r[RSP] = rsp;
    cpu->rip = fn;
    bbcpu_run(cpu, stop);
    return cpu->r[RAX];
}

/* Nested calls (a guest -> host call calling back into the guest) keep the outer state. */
uint64_t bbcpu_call(uintptr_t fn, int count, const uint64_t *args) {
    BbCpu *cpu = current_cpu();
    if (!bbcpu_is_guest_code(fn)) {
        fprintf(stderr, "bbcpu: call of %p, not guest code\n", (void *)fn);
        abort();
    }
    BbCpu saved;
    uint64_t rsp;
    if (cpu->depth == 0) {
        static _Thread_local uint8_t *stack;
        if (!stack) {
            stack = runtime_low_map(GUEST_STACK, PROT_READ | PROT_WRITE);
            if (!stack) abort();
        }
        rsp = (uint64_t)(uintptr_t)(stack + GUEST_STACK - 64);
        cpu->stack_top = (uint64_t)(uintptr_t)(stack + GUEST_STACK);
    } else {
        saved = *cpu;
        rsp = cpu->r[RSP] - RED_ZONE - 64;
    }
    ++cpu->depth;
    const uint64_t result = run_call(cpu, fn, count, args, rsp);
    --cpu->depth;
    if (cpu->depth) {
        const uint64_t instructions = cpu->instructions;
        const uint64_t rax = cpu->r[RAX];
        const BbVec xmm0 = cpu->v[0];
        *cpu = saved;
        cpu->instructions = instructions;
        (void)rax;
        (void)xmm0;
    }
    return result;
}

void bbcpu_call_full(uintptr_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], uint64_t out_gpr[2],
                     uint8_t out_xmm[2][16]) {
    BbCpu *cpu = current_cpu();
    /* bbcpu_call sets the integer arguments; the vector ones go in first (a nested call keeps
     * the outer state, vectors included, and restores it after). */
    if (cpu->depth) {
        BbCpu saved = *cpu;
        for (int i = 0; i < 8; ++i) memcpy(cpu->v[i].b, xmm[i], 16);
        ++cpu->depth;
        out_gpr[0] = run_call(cpu, fn, 6, gpr, cpu->r[RSP] - RED_ZONE - 64);
        --cpu->depth;
        out_gpr[1] = cpu->r[RDX];
        memcpy(out_xmm[0], cpu->v[0].b, 16);
        memcpy(out_xmm[1], cpu->v[1].b, 16);
        const uint64_t instructions = cpu->instructions;
        *cpu = saved;
        cpu->instructions = instructions;
        return;
    }
    for (int i = 0; i < 8; ++i) memcpy(cpu->v[i].b, xmm[i], 16);
    out_gpr[0] = bbcpu_call(fn, 6, gpr);
    out_gpr[1] = cpu->r[RDX];
    memcpy(out_xmm[0], cpu->v[0].b, 16);
    memcpy(out_xmm[1], cpu->v[1].b, 16);
}

uint64_t bbcpu_call_on_stack(uintptr_t fn, int count, const uint64_t *args, void *stack, size_t size) {
    BbCpu *cpu = current_cpu();
    ++cpu->depth;
    cpu->stack_top = (uint64_t)(uintptr_t)stack + size;
    const uint64_t result = run_call(cpu, fn, count, args, (uint64_t)(uintptr_t)stack + size - 64);
    --cpu->depth;
    return result;
}

_Noreturn void bbcpu_fatal(BbCpu *cpu, const BbInsn *in, const char *what) {
    char bytes[64] = {0};
    const uint8_t *code = (const uint8_t *)cpu->rip;
    for (int i = 0; i < (in ? in->length : 8) && i < 15; ++i)
        snprintf(bytes + i * 3, sizeof(bytes) - (size_t)i * 3, "%02x ", code[i]);
    fprintf(stderr, "STOP: bbcpu: %s: %s at guest %#" PRIx64 " (bytes %s)\n", what,
            in ? ZydisMnemonicGetString((ZydisMnemonic)in->mnemonic) : "?", cpu->rip, bytes);
    fprintf(stderr, "  rax=%#" PRIx64 " rbx=%#" PRIx64 " rcx=%#" PRIx64 " rdx=%#" PRIx64
                    " rsi=%#" PRIx64 " rdi=%#" PRIx64 " rbp=%#" PRIx64 " rsp=%#" PRIx64 "\n",
            cpu->r[RAX], cpu->r[RBX], cpu->r[RCX], cpu->r[RDX], cpu->r[RSI], cpu->r[RDI],
            cpu->r[RBP], cpu->r[RSP]);
    fflush(NULL);
    _exit(30);
}

void bbcpu_decode_stats(uint64_t *blocks, uint64_t *insns);
void bbcpu_jit_stats(uint64_t *blocks, uint64_t *fallbacks);
void bbcpu_report(void) {
    uint64_t blocks, insns, translated, fallbacks;
    bbcpu_decode_stats(&blocks, &insns);
    bbcpu_jit_stats(&translated, &fallbacks);
    printf("bbcpu: %" PRIu64 " threads, %" PRIu64 " blocks / %" PRIu64 " instructions decoded, %"
           PRIu64 " blocks translated, %" PRIu64 " instructions interpreted from translated code\n",
           cpus_created, blocks, insns, translated, fallbacks);

}
