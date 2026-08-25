/*
 * ne_mem.h - the memory both halves of a lifted NE share.
 *
 * Deliberately mentions no CPU type. pcrecomp's 16-bit and 32-bit runtimes
 * both define a struct called CPU and both define push32, so they can never
 * appear in one translation unit - which means anything they both need lives
 * here instead.
 *
 * ---- one arena ----
 *
 * The two halves address memory differently. Lifted 32-bit code computes a
 * host address (`rd32(SEGB(c->ds) + off)`); lifted 16-bit code indexes a flat
 * array (`cpu->mem[SEG_OFF(seg, off)]`). They have to mean the same bytes, or
 * a pointer handed from the driver to the decoder points somewhere else.
 *
 * So there is one arena, and two views of it:
 *
 *     g_segoff[sel]   offset of the segment within the arena  (16-bit view)
 *     g_selbase[sel]  host address of the same                (32-bit view)
 *
 * The first 64K of the arena is left unmapped on purpose. An unset selector
 * has offset 0, so reading through one faults immediately instead of quietly
 * returning whatever happens to be at the start of the arena - which is the
 * difference between finding a bug and shipping one.
 */
#ifndef IR32_NE_MEM_H
#define IR32_NE_MEM_H

#include <stdint.h>

#define NE_ARENA_GUARD  0x10000u        /* unmapped, catches selector 0 */
#define NE_ARENA_SIZE   0x2000000u      /* 32 MB: 47 segments plus buffers */

extern unsigned char *g_arena;
extern uint32_t g_segoff[65536];        /* arena-relative, 0 == unmapped */
extern uint32_t g_selbase[65536];       /* host address of the same */
extern uint32_t g_selsize[65536];       /* bytes, 0 == unmapped */

/* Resolve a selector that is not mapped, in case it is one step of a huge
 * pointer. Win16 addresses a block bigger than a segment by adding
 * __AHINCR to the selector for each 64K crossed, so `sel + 8k` names the
 * k-th 64K window of whatever `sel` names. Binding those lazily - only the
 * ones actually used - keeps the selector numbering free of the spacing
 * rules that pre-binding them all would demand.
 *
 * Returns the arena offset, or 0 if the selector really is unmapped. */
uint32_t ne_huge_alias(uint16_t sel);

/* Reserve the arena. Call once, before anything is mapped. */
void ne_mem_init(void);

/* Place `size` bytes for `sel` in the arena and copy `src` into it (or zero it
 * if src is NULL). Returns the arena offset. */
uint32_t ne_alloc(uint16_t sel, const void *src, uint32_t copy, uint32_t size);

/* The automatic data segment's local heap.
 *
 * The NE header asks for one (ne_heap) and the loader is expected to append it
 * past the segment's data - it is where LocalAlloc allocates from, and its
 * handles are NEAR offsets into DS, so it cannot live anywhere else. IR32 asks
 * for 1024 bytes and calls LocalAlloc three times during DRV_OPEN. */
extern uint16_t g_local_base;   /* offset within the data segment */
extern uint16_t g_local_next;
extern uint16_t g_local_end;

/* Which lifted segment a selector holds a copy of, or 0.
 *
 * A 16:32 codec does not call its 32-bit code where the loader put it. It
 * GlobalAllocs a block, copies the code segment into it, marks the descriptor
 * 32-bit through DPMI, and far-calls the COPY - so the call arrives as
 * 0404:2C10 where the lifted functions are registered under segment 3. The
 * copy is byte-identical bar the loader's fixups, so comparing a page of it
 * against each loaded segment identifies it. Answers are cached; a selector
 * that is not a copy of anything is only ever scanned once. */
uint16_t ne_code_alias(uint16_t sel, unsigned nseg);

/* Point a selector at an existing arena offset - for aliasing one segment
 * under a second selector, which the driver does when it hands a buffer on. */
void ne_alias(uint16_t sel, uint32_t arena_off);

#endif /* IR32_NE_MEM_H */
