/*
 * cpu.h - runtime for the DECO_32 static recompilation.
 *
 * Lifted x86 functions operate on a CPU state struct. The recomp runs as a
 * 32-bit program so register values are real 32-bit host pointers: the DLL's
 * data sections live at their original VAs (mapped by the harness/loader) and
 * heap comes from malloc — both inside the 32-bit address space — so memory
 * operands dereference directly.
 *
 * Flags are computed eagerly by helpers after arithmetic/logic ops; Jcc reads
 * the stored bits. This is verbose but mechanical and easy to verify against
 * the hardware. (A lazy-flags optimization can come later.)
 */
#ifndef DECO_CPU_H
#define DECO_CPU_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip;
    /* flags as discrete bits (0/1) */
    uint32_t cf, zf, sf, of, pf, af;
    /* x87: stack of doubles, top index (st0 == st[top]) */
    double   st[8];
    int      fpu_top;
} CPU;

/* ---- partial register access (preserve unaffected bits, like x86) ---- */
#define R8L(r)        ((uint8_t)(r))
#define R8H(r)        ((uint8_t)((r) >> 8))
#define R16(r)        ((uint16_t)(r))
#define SET8L(r, v)   ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SET8H(r, v)   ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))
#define SET16(r, v)   ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))

/* ---- memory access: register holds a real 32-bit address ---- */
static inline uint8_t  rd8 (uint32_t a) { return *(uint8_t  *)(uintptr_t)a; }
static inline uint16_t rd16(uint32_t a) { return *(uint16_t *)(uintptr_t)a; }
static inline uint32_t rd32(uint32_t a) { return *(uint32_t *)(uintptr_t)a; }
static inline void wr8 (uint32_t a, uint8_t  v) { *(uint8_t  *)(uintptr_t)a = v; }
static inline void wr16(uint32_t a, uint16_t v) { *(uint16_t *)(uintptr_t)a = v; }
static inline void wr32(uint32_t a, uint32_t v) { *(uint32_t *)(uintptr_t)a = v; }

/* ---- stack ---- */
static inline void push32(CPU *c, uint32_t v) { c->esp -= 4; wr32(c->esp, v); }
static inline uint32_t pop32(CPU *c) { uint32_t v = rd32(c->esp); c->esp += 4; return v; }

/* ---- absolute image references: abs VA at preferred base -> live address ---- */
extern uint32_t g_image_delta;   /* live_base - 0x11000000 (0 if loaded at preferred base) */
#define GVA(abs) ((uint32_t)((abs) + g_image_delta))

/* ---- flag helpers ---- */
static inline uint32_t parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (~v) & 1;
}

/* logical ops (AND/OR/XOR/TEST): CF=OF=0 */
static inline void flags_logic32(CPU *c, uint32_t r) {
    c->cf = 0; c->of = 0; c->sf = r >> 31; c->zf = (r == 0); c->pf = parity8((uint8_t)r);
}
static inline void flags_logic8(CPU *c, uint8_t r) {
    c->cf = 0; c->of = 0; c->sf = (r >> 7) & 1; c->zf = (r == 0); c->pf = parity8(r);
}
/* cmp/sub (a - b) */
static inline uint32_t flags_sub32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->cf = (a < b);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = (((a ^ b) & (a ^ r)) >> 31) & 1;
    c->af = ((a ^ b ^ r) >> 4) & 1;
    c->pf = parity8((uint8_t)r);
    return r;
}
/* add (a + b) */
static inline uint32_t flags_add32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->cf = (r < a);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    c->af = ((a ^ b ^ r) >> 4) & 1;
    c->pf = parity8((uint8_t)r);
    return r;
}
/* inc/dec: like add/sub by 1 but CF preserved */
static inline uint32_t flags_inc32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_add32(c, a, 1); c->cf = keepcf; return r;
}
static inline uint32_t flags_dec32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_sub32(c, a, 1); c->cf = keepcf; return r;
}

/* ---- shifts (set flags like x86; count masked to 5 bits) ---- */
static inline uint32_t shr32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (cnt - 1)) & 1;
    uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = r >> 31; c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t shl32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (32 - cnt)) & 1;
    uint32_t r = v << cnt;
    c->zf = (r == 0); c->sf = r >> 31; c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint8_t shr8(CPU *c, uint8_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    if (cnt <= 8) c->cf = (v >> (cnt - 1)) & 1;
    uint8_t r = (cnt < 8) ? (uint8_t)(v >> cnt) : 0;
    c->zf = (r == 0); c->sf = (r >> 7) & 1; c->pf = parity8(r);
    return r;
}

#endif /* DECO_CPU_H */
