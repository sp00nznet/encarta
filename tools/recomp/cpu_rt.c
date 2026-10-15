/* cpu_rt.c - shared runtime state for the DECO_32 recompilation. */
#include "cpu.h"

uint32_t g_image_delta = 0;

/* clang-cl's intrin.h declares __writefsdword for x86 but has no builtin for it,
   so lifted SEH prologues (mov fs:[0], esp) link as an undefined
   ___writefsdword. MSVC provides it as an intrinsic. */
#if defined(__clang__) && defined(_M_IX86)
void __writefsdword(unsigned long offset, unsigned long value)
{
    __asm__ volatile("movl %1, %%fs:(%0)" : : "r"(offset), "r"(value) : "memory");
}
#endif
