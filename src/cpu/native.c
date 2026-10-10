/* bbcpu: native versions of game functions (docs/RECOMPILATION.md). A call of a replaced function
 * reaches the dispatcher (the JIT leaves its first instruction untranslated), and bbcpu_step calls
 * the native function through the guest -> host bridge instead. */
#include "trace.h"
#include <stdio.h>
#include <stdlib.h>

enum { BUCKETS = 4096 };
typedef struct Native { uint64_t address; const void *fn; struct Native *next; } Native;
static Native *buckets[BUCKETS];
int bbcpu_native_count;
uint64_t bbcpu_native_calls;

static uint32_t bucket(uint64_t address) { return (uint32_t)((address * 0x9e3779b97f4a7c15ull) >> 52); }

void bbcpu_set_native(uint64_t address, const void *fn) {
    Native *n = malloc(sizeof(*n));
    if (!n) abort();
    *n = (Native){address, fn, buckets[bucket(address)]};
    buckets[bucket(address)] = n;
    ++bbcpu_native_count;
}

const void *bbcpu_native_at(uint64_t address) {
    for (const Native *n = buckets[bucket(address)]; n; n = n->next)
        if (n->address == address) return n->fn;
    return NULL;
}
