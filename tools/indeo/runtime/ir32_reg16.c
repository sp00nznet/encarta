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
#include "ne_mem.h"

/* Where the last driver message faulted. An exception code on its own does not
 * distinguish a wild pointer from running one byte off a buffer, and those
 * want opposite investigations. */
static uint32_t g_msg_fault_at;
static int g_msg_fault_write;

static int msg_fault(EXCEPTION_POINTERS *ep)
{
    g_msg_fault_at = 0;
    g_msg_fault_write = 0;
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        g_msg_fault_write = (int)ep->ExceptionRecord->ExceptionInformation[0];
        g_msg_fault_at = (uint32_t)ep->ExceptionRecord->ExceptionInformation[1];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
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

/* ---- calling the driver the way Video for Windows does ----------------
 *
 * DriverProc is FAR PASCAL:
 *
 *   LRESULT DriverProc(DWORD dwDriverID, HDRVR hDriver, UINT msg,
 *                      LPARAM lParam1, LPARAM lParam2)
 *
 * Pascal pushes left to right, so the last argument is nearest the return
 * address. The lifted code confirms the layout rather than the documentation
 * having to be taken on trust - it does `enter 0x2A` and then reads
 *
 *   [bp+0x12] dwDriverID   [bp+0x0E] msg      [bp+0x06] lParam2
 *   [bp+0x10] hDriver      [bp+0x0A] lParam1
 *
 * which is exactly 16 bytes of arguments above a 4-byte far return address
 * and the saved BP. The result comes back in DX:AX.
 */
uint32_t ir32_driver_call(uint32_t driver_id, uint16_t hdrv, uint16_t msg,
                          uint32_t lp1, uint32_t lp2,
                          uint16_t ds, uint16_t ss, uint16_t sp)
{
    extern unsigned char *g_arena;
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = g_arena;
    cpu.ds = ds;
    cpu.es = ds;
    cpu.ss = ss;
    cpu.sp = sp;
    cpu.cs = 6;

    push32(&cpu, driver_id);
    push16(&cpu, hdrv);
    push16(&cpu, msg);
    push32(&cpu, lp1);
    push32(&cpu, lp2);
    /* 0xFFFF is lift16's marker for a return address it does not know, and
     * recomp_dispatch treats it as the end of a call. Pushing zero instead
     * makes DriverProc's own `retf` dispatch to 0000:0000, which then reads as
     * a missing function rather than as a return. */
    push16(&cpu, 0xFFFF);   /* return segment */
    push16(&cpu, 0xFFFF);   /* return offset */
    cpu.bp = cpu.sp;

#if defined(_WIN32)
    __try {
        recomp_dispatch(&cpu, 6, 0);
    } __except (msg_fault(GetExceptionInformation())) {
        printf("   msg %04X: faulted (0x%08lX) %s %08X",
               msg, (unsigned long)GetExceptionCode(),
               g_msg_fault_write ? "writing" : "reading", g_msg_fault_at);
        /* An address alone says nothing here, because every object lives at
         * an arbitrary offset in one arena. Name the selector it belongs to -
         * that is the difference between "it faulted" and "it ran off the end
         * of the output buffer". */
        {
            uint32_t base = (uint32_t)(uintptr_t)g_arena;
            uint32_t rel = g_msg_fault_at - base;
            printf("  arena+%d", (int32_t)rel);
            uint16_t best = 0; uint32_t bestoff = 0xFFFFFFFFu;
            for (unsigned s = 1; s < 0x10000; s++)
                if (g_segoff[s] && g_segoff[s] <= rel && rel - g_segoff[s] < bestoff) {
                    bestoff = rel - g_segoff[s];
                    best = (uint16_t)s;
                }
            if (best)
                printf("  = %04X:%08X", best, bestoff);
        }
        printf("\n");
        return 0xFFFFFFFFu;
    }
#else
    recomp_dispatch(&cpu, 6, 0);
#endif
    return ((uint32_t)cpu.dx << 16) | cpu.ax;
}
