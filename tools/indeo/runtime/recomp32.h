/*
 * recomp32.h - runtime for lifted 32-bit NE code (Indeo's IR32.DLL).
 *
 * The CPU model itself is pcrecomp's: include cpu.h and use its registers,
 * flags and helpers unchanged. What an NE needs on top is a memory model,
 * because a PE has one address space and an NE has one per segment.
 *
 * ---- addresses ----
 *
 * A register here holds an OFFSET WITHIN A SEGMENT, not an address. The 32-bit
 * code says `mov eax, [0xE1A8]` meaning offset 0xE1A8 in DS, and `mov bh,
 * gs:[ebp+1]` meaning an offset in whatever GS the caller passed. So every
 * lifted access is emitted with its segment and resolves as
 *
 *     rd32(SEGB(c->ds) + offset)
 *
 * SEGB maps a selector to the host address the segment was loaded at. The
 * build is 32-bit - the same assumption cpu.h already documents - so a host
 * address fits in uint32_t and cpu.h's rd/wr dereference it directly. No
 * emulated address space, no translation on every access beyond one lookup.
 *
 * The stack is the exception, and deliberately: push32/pop32 in cpu.h use
 * c->esp raw, with no segment. That is consistent as long as the stack
 * selector's base is zero, so ne_init() allocates the stack and registers it
 * that way, leaving ESP a flat host address. A real 16-bit caller would pass a
 * based SS; here the caller is us.
 *
 * ---- what the segments are ----
 *
 * DS   the DLL's own data segment: decoder state, tables
 * ES   the output frame, handed in by the caller
 * GS   the compressed bitstream, handed in by the caller
 * CS   which code segment is executing - dispatch() needs it to know which
 *      entry table an offset belongs to
 *
 * FS also appears, and is NOT the thread block. IR32 keeps decoder state in an
 * FS-addressed segment, so the __readfsdword path that pcrecomp's lifter uses
 * for PE SEH prologues is wrong here and is overridden in lift_ir32.py.
 */
#ifndef IR32_RECOMP32_H
#define IR32_RECOMP32_H

#include <stdint.h>
#include <stdlib.h>   /* abort(): the lifter emits it for gaps */

/* The stack is a segment like any other, so ESP is an offset within it and
 * push/pop have to add the base. This must be defined before cpu.h, which
 * otherwise assumes a flat stack. It is why ESP here is a small number and not
 * a host address: the code does `mov bp, sp` and then reads `[bp+6]`, and a
 * host address does not survive being truncated to 16 bits. */
extern uint32_t g_selbase[65536];
#define SEGB(sel) (g_selbase[(uint16_t)(sel)])
#define STACK_BASE(c) SEGB((c)->ss)

#include "cpu.h"

/* ---- selector -> base ------------------------------------------------- */

/* Indexed by selector. 64K entries of uint32_t is 256 KB, which is not worth
 * being clever about to save. A selector with no segment mapped reads 0, and
 * an access through it faults immediately at a near-null address rather than
 * silently landing somewhere plausible. */
/* ---- lifted code ------------------------------------------------------ */

typedef struct {
    uint32_t off;              /* offset within the segment */
    void (*fn)(CPU *c);        /* the lifted function starting there */
} ne_entry;

/* Register a segment's lifted entry points under the selector its code runs
 * with. dispatch() uses c->cs to pick between them. */
void ne_register_code(uint16_t sel, const ne_entry *entries, unsigned count);

/* Map a segment's bytes at `host` and give it a selector. */
void ne_map(uint16_t sel, void *host, uint32_t size);

/* Call the lifted function at `target` in the current code segment. */
void dispatch(CPU *c, uint32_t target);

/* Same, for an indirect jump: the callee returns to our caller, not to us. */
void dispatch_jmp(CPU *c, uint32_t target);

/* Set up the selector table and a stack. Returns the stack selector. */
uint16_t ne_init(uint32_t stack_bytes);

/* Diagnostics: how many dispatches missed the entry table, and where. */
extern unsigned long g_dispatch_misses;
extern uint32_t g_last_miss;

/* Set to keep going past a miss instead of aborting. Wrong execution, but it
 * lets a survey measure every entry rather than stopping at the first. */
extern int g_dispatch_soft;
void ne_report_misses(void);

#endif /* IR32_RECOMP32_H */
