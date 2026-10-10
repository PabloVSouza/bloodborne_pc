/* bbcpu: recompiled game functions (docs/RECOMPILATION.md). BB_RECOMP_LIB names a library of C
 * written by tools/recomp/bbrecomp.c from the player's own eboot (src/recomp/rc.h): its functions
 * replace the translated ones. A call of one reaches the dispatcher (the JIT leaves its first
 * instruction untranslated) and bbcpu_step runs it. BB_RECOMP_OFF=OFFSET,... leaves chosen ones to
 * the translator (to find a wrong one by bisection). */
#include "trace.h"
#include "../recomp/rc.h"
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

enum { BUCKETS = 16384 };
typedef struct Recomp { uint64_t address; RcFn fn; struct Recomp *next; } Recomp;
static Recomp *buckets[BUCKETS];
int bbcpu_recomp_count;
uint64_t bbcpu_recomp_calls;

static uint32_t bucket(uint64_t address) { return (uint32_t)((address * 0x9e3779b97f4a7c15ull) >> 50); }

RcFn bbcpu_recomp_at(uint64_t address) {
    for (const Recomp *r = buckets[bucket(address)]; r; r = r->next)
        if (r->address == address) return r->fn;
    return NULL;
}

static void add(uint64_t address, RcFn fn) {
    Recomp *r = malloc(sizeof(*r));
    if (!r) abort();
    *r = (Recomp){address, fn, buckets[bucket(address)]};
    buckets[bucket(address)] = r;
    ++bbcpu_recomp_count;
}

int bbcpu_is_tcb_load(const BbInsn *in) {
    if (in->mnemonic != ZYDIS_MNEMONIC_MOV || in->count != 2) return 0;
    const BbOp *d = &in->op[0], *s = &in->op[1];
    return d->type == OP_REG && d->kind == RK_GPR && d->size == 8 && s->type == OP_MEM && s->size == 8 &&
           s->base == 0xff && s->index == 0xff && ((s->segment == 1 && s->disp == 0) || s->segment == 2);
}

uint64_t bbcpu_recomp_hash(uint64_t start, uint64_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
#define MIX(byte) (h = (h ^ (uint8_t)(byte)) * 0x100000001b3ull)
    for (uint64_t at = start; at < start + size;) {
        const BbBlock *block = bbcpu_block(at);
        if (!block->count) break;
        for (uint32_t i = 0; i < block->count && at < start + size; ++i) {
            const BbInsn *in = &block->insn[i];
            if (bbcpu_is_tcb_load(in)) {
                MIX(0xf5);
                MIX(in->op[0].reg);
            } else {
                for (int b = 0; b < in->length; ++b) MIX(((const uint8_t *)at)[b]);
            }
            at += in->length;
        }
    }
#undef MIX
    return h;
}

/* BB_RECOMP_THREADS / BB_RECOMP_NOT_THREADS=NAME,... (diagnostics): recompiled functions only on
 * the threads whose name contains one of the first, and none of the second (host thread names). */
static int name_matches(const char *list, const char *name) {
    if (!list) return 0;
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", list);
    for (char *part = strtok(copy, ","); part; part = strtok(NULL, ","))
        if (*part && strstr(name, part)) return 1;
    return 0;
}
int bbcpu_recomp_thread_ok(void) {
    static _Thread_local int ok = -1;
    if (ok < 0) {
        const char *only = getenv("BB_RECOMP_THREADS"), *not = getenv("BB_RECOMP_NOT_THREADS");
        if (!only && !not) {
            ok = 1;
        } else {
            char name[64] = "";
            pthread_getname_np(pthread_self(), name, sizeof(name));
            ok = (!only || name_matches(only, name)) && !name_matches(not, name);
        }
    }
    return ok;
}

/* ---- the runtime the generated code calls (RcApi) ---- */

/* The return address of calls run in the translator (never guest code): bbcpu_run stops there. */
#define SENTINEL UINT64_C(0x0000700000000e00)

static uint64_t rc_step(BbCpu *cpu) {
    const BbBlock *block = bbcpu_block(cpu->rip);
    return bbcpu_step_insn(cpu, &block->insn[0]);
}

/* BB_RECOMP_NODIRECT=1 (diagnostics): calls and tail calls between recompiled functions go
 * through the translator's dispatcher instead of being made directly. */
static int no_direct(void) {
    static int value = -1;
    if (value < 0) {
        const char *env = getenv("BB_RECOMP_NODIRECT");
        value = env && env[0] == '1';
    }
    return value;
}

static void rc_call(BbCpu *cpu, uint64_t target, uint64_t next) {
    const RcFn fn = no_direct() || !bbcpu_recomp_thread_ok() ? NULL : bbcpu_recomp_at(target);
    cpu->r[RSP] -= 8;
    if (fn) {
        __atomic_add_fetch(&bbcpu_recomp_calls, 1, __ATOMIC_RELAXED);
        bb_store(cpu->r[RSP], 8, next);
        cpu->rip = target;
        fn(cpu);
        return;
    }
    /* The translator (and natives, imports: bbcpu_run calls them), returning to SENTINEL. */
    bb_store(cpu->r[RSP], 8, SENTINEL);
    cpu->rip = target;
    bbcpu_run(cpu, SENTINEL);
    cpu->rip = next;
}

static void rc_tail(BbCpu *cpu) {
    const RcFn fn = no_direct() || !bbcpu_recomp_thread_ok() ? NULL : bbcpu_recomp_at(cpu->rip);
    if (fn) {
        __atomic_add_fetch(&bbcpu_recomp_calls, 1, __ATOMIC_RELAXED);
        fn(cpu);
        return;
    }
    const uint64_t back = bb_load(cpu->r[RSP], 8);
    bb_store(cpu->r[RSP], 8, SENTINEL);
    bbcpu_run(cpu, SENTINEL);
    cpu->rip = back;
}

static void rc_bail(BbCpu *cpu, uint64_t entry_rsp) {
    const uint64_t back = bb_load(entry_rsp, 8);
    bb_store(entry_rsp, 8, SENTINEL);
    bbcpu_run(cpu, SENTINEL);
    cpu->rip = back;
}

/* Calls of recompiled functions per second, every 10 s while there are any. */
static void *report(void *unused) {
    (void)unused;
    for (uint64_t last = 0;;) {
        sleep(10);
        const uint64_t now = __atomic_load_n(&bbcpu_recomp_calls, __ATOMIC_RELAXED);
        if (now != last) printf("Recompiled: %.0f calls/s\n", (double)(now - last) / 10.0);
        last = now;
    }
    return NULL;
}

static int switched_off(const char *list, uint64_t offset) {
    for (const char *p = list; p && *p;) {
        char *end;
        const uint64_t value = strtoull(p, &end, 0);
        if (end == p) break;
        if (value == offset) return 1;
        p = *end == ',' ? end + 1 : end;
    }
    return 0;
}

void bbcpu_recomp_load(uint64_t image_base) {
    const char *path = getenv("BB_RECOMP_LIB");
    if (!path || !*path) return;
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const RcInit init = library ? (RcInit)dlsym(library, BB_RECOMP_INIT) : NULL;
    if (!init) {
        fprintf(stderr, "Recompiled: %s: %s\n", path, dlerror());
        return;
    }
    static RcApi api;
    api = (RcApi){.version = BB_RECOMP_API_VERSION, .image_base = image_base, .step = rc_step,
                  .call = rc_call, .tail = rc_tail, .bail = rc_bail};
    size_t count = 0;
    const RcFunction *functions = init(&api, &count);
    const char *off = getenv("BB_RECOMP_OFF");
    size_t used = 0, changed = 0;
    for (size_t i = 0; i < count; ++i) {
        if (switched_off(off, functions[i].offset)) continue;
        if (bbcpu_recomp_hash(image_base + functions[i].offset, functions[i].size) != functions[i].hash) {
            ++changed; /* patched since (game patches, hooks): the translator runs it */
            continue;
        }
        add(image_base + functions[i].offset, functions[i].fn);
        ++used;
    }
    printf("Recompiled: %zu of %zu game functions replaced, %zu changed by patches (%s)\n", used, count,
           changed, path);
    pthread_t thread;
    if (used && pthread_create(&thread, NULL, report, NULL) == 0) pthread_detach(thread);
}
