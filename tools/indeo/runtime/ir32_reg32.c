/*
 * ir32_reg32.c - the 32-bit half: registration, and the tests that need a
 * 32-bit machine.
 *
 * Separate from ir32_reg16.c for the same reason that file explains, and it
 * carries the init and sweep tests because both build a CPU, which only a file
 * including the 32-bit runtime can do.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp32.h"
#include "ir32_segs.h"
#if defined(_WIN32)
#include <windows.h>
#include <malloc.h>
#endif

#define DECL32(n) \
    extern const ne_entry ir32_seg##n##_entries[]; \
    extern const unsigned ir32_seg##n##_entry_count;
IR32_SEGS32(DECL32)
#undef DECL32

void ir32_register32(void)
{
#define REG32(n) ne_register_code(n, ir32_seg##n##_entries, \
                                  ir32_seg##n##_entry_count);
    IR32_SEGS32(REG32)
#undef REG32
}

/* 3:0000 is the decoder's initialisation and the one entry that needs nothing
 * but somewhere to work: its only argument is an instance segment's selector.
 * The values checked afterwards were read off the instructions before the code
 * was ever run, which is the only reason they are worth checking:
 *
 *   mov ebx, 0xE20C / mov ecx, 0xB0
 *   loop: store 8, ebx += 8, ecx -= 8, jg loop   ->  ebx = 0xE20C + 176
 *   add ebx, 4     -> 0xE2C0   mov es:[0x0C], ebx
 *   add ebx, 0x2C  -> 0xE2EC   mov es:[0x18], ebx
 */
int ir32_init_test(uint16_t ds, uint16_t ss)
{
    enum { INST_SEL = 0x0200, INST_SIZE = 0x10000 };
    ne_alloc(INST_SEL, NULL, 0, INST_SIZE);
    unsigned char *inst = g_arena + g_segoff[INST_SEL];

    CPU c;
    memset(&c, 0, sizeof c);
    c.cs = 3;
    c.ds = ds;
    c.es = INST_SEL;
    c.ss = ss;
    c.esp = 0xFF00u;
    wr16(SEGB(c.ss) + c.esp + 4, INST_SEL);   /* argument at [bp+4] */
    c.ebp = c.esp;

    printf("calling 3:0000 (init) ...\n");
    dispatch(&c, 0x00000000u);

    unsigned fill = 0;
    for (unsigned o = 0xE20C; o + 4 <= 0xE20C + 0xB0; o += 4)
        if (inst[o] == 0x40 && inst[o+1] == 0x40 &&
            inst[o+2] == 0x40 && inst[o+3] == 0x40)
            fill += 4;
    uint32_t p0C = *(uint32_t *)(inst + 0x0C);
    uint32_t p18 = *(uint32_t *)(inst + 0x18);
    printf("fill at 0xE20C: %u of %u bytes are 0x40404040\n", fill, 0xB0);
    printf("pointers written: [0x0C]=%08X [0x18]=%08X\n", p0C, p18);

    int wrong = 0;
    if (fill != 0xB0)        wrong |= 1;
    if (p0C != 0xE2C0u)      wrong |= 2;
    if (p18 != 0xE2ECu)      wrong |= 4;
    if (g_dispatch_misses)   wrong |= 8;
    printf("init check: %s\n", wrong ? "FAILED" : "matches the disassembly");
    return wrong;
}

/* Call every lifted 32-bit entry with a blank machine. This measures the
 * runtime and not the decoder: most of these want arguments, and reading
 * through an unset selector is exactly the fault the guard page exists to
 * produce. What it establishes is that dispatch resolves and the bases hold. */
int ir32_sweep32(uint16_t ds, uint16_t ss)
{
    unsigned ok = 0, faulted = 0, overflowed = 0;
    g_dispatch_soft = 1;
    for (unsigned i = 0; i < ir32_seg3_entry_count; i++) {
        CPU c;
        memset(&c, 0, sizeof c);
        c.cs = 3;
        c.ds = ds; c.es = ds; c.fs = ds; c.gs = ds;
        c.ss = ss;
        c.esp = 0xFF00u;
        c.ebp = c.esp;
#if defined(_WIN32)
        __try {
            ir32_seg3_entries[i].fn(&c);
            ok++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* A stack overflow is not like the other faults.
             * Reconnecting the functions that run on into the next one
             * turns a loop spanning two carved functions into C
             * recursion, and a blank machine has no loop counter to end
             * it - so some entries run the stack out rather than reading
             * through an unset selector. The guard page is consumed when
             * that happens, and without _resetstkoflw the NEXT overflow
             * takes the process down without reaching this handler, which
             * is why the whole sweep died instead of one entry failing. */
            if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW) {
                _resetstkoflw();
                overflowed++;
            } else {
                faulted++;
            }
        }
#else
        ir32_seg3_entries[i].fn(&c);
        ok++;
#endif
    }
    printf("sweep: %u of %u entries returned, %u faulted, %u ran the stack out\n",
           ok, ir32_seg3_entry_count, faulted, overflowed);
    ne_report_misses();
    return 0;
}
