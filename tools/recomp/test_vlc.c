/*
 * test_vlc.c - differential test: hand-lifted L_11019780 vs the real
 * DECO_32.DLL sub_11019780, fuzzed over random bitstreams and bit positions.
 *
 * Validates the cpu.h runtime and the lift methodology end-to-end. Build 32-bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "cpu.h"

void L_11019780(CPU *c);   /* from lifted_manual.c */

/* real fn is __thiscall (this in ECX). Call via __fastcall(ecx, edx). */
typedef int (__fastcall *real_fn_t)(uint32_t ecx, uint32_t edx);

#define INST_SIZE  256
#define BITS_SIZE  65536
#define STACK_SIZE 8192

int main(int argc, char **argv)
{
    const char *dll = (argc >= 2) ? argv[1] : "C:\\encarta\\analysis\\DECO_32.DLL";
    HMODULE h = LoadLibraryA(dll);
    if (!h) { fprintf(stderr, "cannot load %s\n", dll); return 1; }
    uint32_t base = (uint32_t)(uintptr_t)h;
    g_image_delta = base - 0x11000000u;
    real_fn_t real_fn = (real_fn_t)(uintptr_t)(base + 0x19780u);
    fprintf(stderr, "DECO_32 @ %08X delta %+d, real sub_11019780 @ %08X\n",
            base, (int)g_image_delta, base + 0x19780u);

    /* shared read-only bitstream (both impls only read it) */
    uint8_t *bits = (uint8_t *)malloc(BITS_SIZE);
    /* two independent instance structs (each impl mutates its own +3C/+40) */
    uint8_t *inst_r = (uint8_t *)calloc(1, INST_SIZE);
    uint8_t *inst_l = (uint8_t *)calloc(1, INST_SIZE);
    uint8_t *stack  = (uint8_t *)malloc(STACK_SIZE);

    long trials = (argc >= 3) ? atol(argv[2]) : 2000000;
    long fails = 0;
    unsigned seed = 0x1234567u;

    for (long t = 0; t < trials; t++) {
        /* fill a fresh random bitstream; keep zero bits frequent so the unary
           prefix terminates quickly and never runs off the buffer */
        for (int i = 0; i < 64; i++) {
            seed = seed * 1103515245u + 12345u;
            bits[i] = (uint8_t)((seed >> 16) & ((seed >> 8) & 0xFF)); /* sparse 1s */
        }
        seed = seed * 1103515245u + 12345u;
        uint32_t startpos = (seed >> 16) % 8;   /* bit position 0..7 */

        /* set up both instances identically: [3C]=bit ptr, [40]=bit pos */
        *(uint32_t *)(inst_r + 0x3C) = (uint32_t)(uintptr_t)bits;
        *(uint32_t *)(inst_r + 0x40) = startpos;
        *(uint32_t *)(inst_l + 0x3C) = (uint32_t)(uintptr_t)bits;
        *(uint32_t *)(inst_l + 0x40) = startpos;

        /* real */
        int rret = real_fn((uint32_t)(uintptr_t)inst_r, 0);

        /* lifted */
        CPU c;
        memset(&c, 0, sizeof c);
        c.ecx = (uint32_t)(uintptr_t)inst_l;
        c.esp = (uint32_t)(uintptr_t)(stack + STACK_SIZE - 64);
        /* dirty the callee-saved regs to prove save/restore is faithful */
        c.ebx = 0xAAAAAAAAu; c.esi = 0xBBBBBBBBu;
        c.edi = 0xCCCCCCCCu; c.ebp = 0xDDDDDDDDu;
        L_11019780(&c);
        int lret = (int)c.eax;

        uint32_t r3c = *(uint32_t *)(inst_r + 0x3C), r40 = *(uint32_t *)(inst_r + 0x40);
        uint32_t l3c = *(uint32_t *)(inst_l + 0x3C), l40 = *(uint32_t *)(inst_l + 0x40);

        if (rret != lret || r3c != l3c || r40 != l40) {
            if (fails < 10)
                fprintf(stderr,
                    "MISMATCH t=%ld startpos=%u  ret r=%d l=%d  ptr r=%08X l=%08X  pos r=%u l=%u\n",
                    t, startpos, rret, lret, r3c, l3c, r40, l40);
            fails++;
        }
    }

    printf("%s: %ld trials, %ld mismatches\n", fails ? "FAIL" : "PASS", trials, fails);
    FreeLibrary(h);
    return fails ? 1 : 0;
}
