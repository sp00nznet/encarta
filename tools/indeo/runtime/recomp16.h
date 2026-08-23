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
#include "ne_mem.h"

#define SEG_OFF(seg, off) (g_segoff[(uint16_t)(seg)] + (uint32_t)(uint16_t)(off))

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
void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off);

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
