/* bbcpu: running one guest function instruction by instruction with its memory accesses seen
 * (record.c): the recorder of BB_RECORD and tools/decomp/replay.c. */
#ifndef BB_CPU_TRACE_H
#define BB_CPU_TRACE_H
#include "cpu_internal.h"
#include "record.h"

/* Memory accesses: PRE before the instruction runs (its operands), POST after it (the same
 * operands: changed bytes are writes); READ and WRITE after a string instruction. */
enum { BB_TRACE_PRE, BB_TRACE_POST, BB_TRACE_READ, BB_TRACE_WRITE };
/* How the function leaves its code: a call, a tail call (jump out of [start, end)), an int3 hook. */
enum { BB_TRACE_CALL, BB_TRACE_TAIL, BB_TRACE_TRAP };

typedef struct BbTrace BbTrace;
struct BbTrace {
    BbCpu *cpu;
    uint64_t start, end; /* the function's code */
    uint64_t entry_rsp;
    uint64_t instructions;
    uint64_t limit; /* stop after this many instructions (0: none); `stopped` is then set */
    int stopped;
    void (*access)(BbTrace *t, int kind, uint64_t address, uint32_t length);
    /* Runs the call (CALL: cpu->rip is the return point; TAIL: the function's return address is
     * on the stack; TRAP: cpu->rip is the int3) and leaves the cpu where the function resumes. */
    void (*call)(BbTrace *t, const BbInsn *in, uint64_t target, int how);
    /* An instruction whose accesses the tracer cannot list (the record is not usable). */
    void (*unsupported)(BbTrace *t, const BbInsn *in);
};

/* Runs the function at cpu->rip (its return address on top of the stack) until it returns. */
void bbcpu_trace(BbTrace *t);
void bbcpu_trace_regs(const BbCpu *cpu, BbRecRegs *regs);
void bbcpu_trace_set_regs(BbCpu *cpu, const BbRecRegs *regs);

/* BB_RECORD (record.c): whether `rip` starts a function being recorded (the JIT leaves its first
 * instruction to bbcpu_step), whether this call is to be recorded, and the recording itself
 * (returns the rip to continue at, after the function returned). */
extern int bbcpu_record_armed;
int bbcpu_record_target(uint64_t rip);
int bbcpu_record_wants(uint64_t rip);
uint64_t bbcpu_record_call(BbCpu *cpu);

#endif
