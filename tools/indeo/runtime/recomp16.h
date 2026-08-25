/*
 * recomp16.h - runtime for the lifted 16-bit half of an NE.
 *
 * pcrecomp's 16-bit runtime was written for DOS, where a segment means
 * `seg << 4` into a 1 MB array. A Windows NE is protected mode: a selector is
 * an index into a descriptor table and its numeric value says nothing about
 * where the segment is. That is the only thing that has to change, and cpu.h
 * now lets it be replaced - define SEG_OFF and every accessor, and every
 * lifted `mem_read16(cpu, cpu->ds, off)`, follows.
 *
 * The arena is shared with the 32-bit half (see ne_mem.h), so a pointer the
 * driver builds and hands to the decoder means the same bytes on both sides.
 */
#ifndef IR32_RECOMP16_H
#define IR32_RECOMP16_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "ne_mem.h"

/* An unmapped selector has base 0, so an access through one lands in the
 * arena's first 64K - left unmapped on purpose so it faults instead of
 * quietly hitting a neighbour. The fault address alone does not say WHICH
 * selector, and that is the part worth knowing, so name it at first use.
 * IR32_SEGGUARD keeps it out of ordinary runs. */
static inline uint32_t ir32_seg_off(uint16_t seg, uint16_t off)
{
    if (!g_segoff[seg] && seg) {
        /* Might be one step of a huge pointer rather than a bad selector. */
        if (!ne_huge_alias(seg)) {
            static unsigned char said[65536];
            if (!said[seg] && getenv("IR32_SEGGUARD")) {
                said[seg] = 1;
                fprintf(stderr, "   selector %04X is not mapped "
                                "(first access at offset %04X)\n", seg, off);
            }
        }
    }
    return g_segoff[seg] + (uint32_t)off;
}
#define SEG_OFF(seg, off) ir32_seg_off((uint16_t)(seg), (uint16_t)(off))

/* By directory, not by name: pcrecomp has runtime/recomp16/cpu.h and
 * runtime/recomp32_cpu/cpu.h, and an include path that can see both
 * resolves plain "cpu.h" to whichever comes first. */
#include "recomp16/cpu.h"

/* A far call or jump, by selector and offset.
 *
 * The target may be in either half. Segments 2 and 3 are 32-bit, and calling
 * one means building a 32-bit machine, copying the arguments the 16-bit caller
 * already pushed, and running it - which is what the real thunks do, only in
 * hardware. That bridge lives in its own translation unit because the two
 * runtimes cannot share one. */
/* Declared here too: the 16-bit half calls across the boundary and cannot
 * include recomp32.h, which would drag in the other CPU type. */
typedef struct {
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
} ne_regs;
unsigned ne_call32(uint16_t seg, uint32_t off, uint16_t ss, uint16_t sp,
                   uint16_t ds, uint16_t es, const ne_regs *r);

void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off);

/* An indirect far call - `call far word ds:[bx]` - reads its target from
 * memory, so lift16 cannot name a function and emits this instead. It is a
 * far call like any other: the selector and offset are already resolved by
 * the time they get here, and the same dispatch has to route them, because
 * the target may be in either half of the DLL.
 *
 * Declared here rather than left to the linker: without it the generated
 * code names a symbol nothing defines, and the failure is a link error
 * listing four segments with no hint that the cause is one missing
 * one-line thunk. */
static inline void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off)
{
    recomp_dispatch(cpu, seg, off);
}

typedef struct {
    uint16_t off;
    void (*fn)(CPU *cpu);
} ne16_entry;

void ne_register_code16(uint16_t sel, const ne16_entry *entries, unsigned count);

/* Win16 imports, by module and ordinal. Nothing here implements Windows; the
 * point is to find out empirically which of the 61 imports a decode actually
 * reaches, rather than guessing at all of them up front. */
void ne_import(CPU *cpu, const char *module, uint16_t ordinal);
void ne_report_imports(void);

extern unsigned long g_dispatch16_misses;

#endif /* IR32_RECOMP16_H */
