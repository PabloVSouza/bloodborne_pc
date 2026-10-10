/* Native versions of the game's functions (docs/DECOMPILATION.md): BB_NATIVE_LIB names a library
 * (src/native/bbnative.h) whose functions replace the game's. BB_NATIVE_OFF=OFFSET,... leaves
 * chosen ones to the game's code (to find a wrong one by bisection). */
#include "runtime.h"
#include "cpu/bbcpu.h"
#include "native/bbnative.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BB_HAVE_BBCPU
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

extern uint64_t bbcpu_native_calls;

/* Native calls per second, every 10 s while there are any. */
static void *report(void *unused) {
    (void)unused;
    for (uint64_t last = 0;;) {
        sleep(10);
        const uint64_t now = __atomic_load_n(&bbcpu_native_calls, __ATOMIC_RELAXED);
        if (now != last) printf("Native: %.0f calls/s\n", (double)(now - last) / 10.0);
        last = now;
    }
    return NULL;
}

static void call_game(uint64_t fn, const BbCallIn *in, BbCallOut *out) {
    uint64_t gpr[2];
    uint8_t xmm[2][16];
    bbcpu_call_full((uintptr_t)fn, in->gpr, in->xmm, gpr, xmm);
    out->rax = gpr[0];
    out->rdx = gpr[1];
    memcpy(out->xmm0, xmm[0], 16);
    memcpy(out->xmm1, xmm[1], 16);
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

void runtime_native_load(uint64_t image_base) {
    const char *path = getenv("BB_NATIVE_LIB");
    if (!path || !*path) return;
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    BbNativeInit init = library ? (BbNativeInit)dlsym(library, BB_NATIVE_INIT) : NULL;
    if (!init) {
        fprintf(stderr, "Native: %s: %s\n", path, dlerror());
        return;
    }
    static BbNativeApi api;
    api = (BbNativeApi){.version = BB_NATIVE_API_VERSION, .image_base = image_base, .call = call_game};
    size_t count = 0;
    const BbNativeFunction *functions = init(&api, &count);
    const char *off = getenv("BB_NATIVE_OFF");
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (switched_off(off, functions[i].offset)) continue;
        bbcpu_set_native(image_base + functions[i].offset, functions[i].function);
        ++used;
    }
    printf("Native: %zu of %zu game functions replaced (%s)\n", used, count, path);
    pthread_t thread;
    if (used && pthread_create(&thread, NULL, report, NULL) == 0) pthread_detach(thread);
}
#else
void runtime_native_load(uint64_t image_base) { (void)image_base; }
#endif
