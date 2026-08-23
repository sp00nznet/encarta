/*
 * ir32_reg16.c - declare and register the lifted 16-bit segments.
 *
 * Its own translation unit because it includes the 16-bit runtime, and the two
 * runtimes cannot meet: both define a struct called CPU and a function called
 * push32. The list comes from ir32_segs.h, which lift_all.py generates, so a
 * segment cannot be lifted and then quietly not registered.
 */
#include <stdio.h>
#include <string.h>
#include "recomp16.h"
#include "ir32_segs.h"
#if defined(_WIN32)
#include <windows.h>
#endif

#define DECL16(n) \
    extern const ne16_entry ir32_seg##n##_entries[]; \
    extern const unsigned ir32_seg##n##_entry_count;
IR32_SEGS16(DECL16)
#undef DECL16

void ir32_register16(void)
{
#define REG16(n) ne_register_code16(n, ir32_seg##n##_entries, \
                                    ir32_seg##n##_entry_count);
    IR32_SEGS16(REG16)
#undef REG16
}

/* Call a lifted 16-bit entry with a machine set up the way the driver's caller
 * would leave it. Returns the far-call dispatch miss count, so a caller in a
 * translation unit that cannot see CPU still learns whether it worked. */
unsigned long ir32_call16(uint16_t seg, uint16_t off, uint16_t ds,
                          uint16_t ss, uint16_t sp)
{
    extern unsigned char *g_arena;
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = g_arena;
    cpu.cs = seg;
    cpu.ds = ds;
    cpu.es = ds;
    cpu.ss = ss;
    cpu.sp = sp;
    cpu.bp = sp;
#if defined(_WIN32)
    /* Catch the fault rather than letting it take the process down. A run that
     * dies loses the import report, which is the part worth having: what the
     * 16-bit half reached before it stopped says more about what is missing
     * than the fault address does. */
    __try {
        recomp_dispatch(&cpu, seg, off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("faulted in %04X:%04X (code 0x%08lX) after %lu far calls\n",
               cpu.cs, cpu.ip, (unsigned long)GetExceptionCode(),
               g_dispatch16_misses);
    }
#else
    recomp_dispatch(&cpu, seg, off);
#endif
    return g_dispatch16_misses;
}
