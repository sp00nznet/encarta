/*
 * lifted_manual.c - hand-lifted DECO_32 functions (mechanical, 1:1 with asm).
 *
 * These are translated instruction-by-instruction from the disassembly to
 * validate the cpu.h runtime and the differential harness BEFORE the codegen
 * (lift.py) is automated. Each line maps to one x86 instruction; the address
 * comment is the original EA.
 */
#include "cpu.h"

uint32_t g_image_delta = 0;

/* sub_11019780 - VLC / Exp-Golomb count reader (__thiscall, this in ECX).
 * Reads a unary prefix of 1-bits, then that many suffix bits, from the
 * bitstream pointed to by [this+0x3C] at bit position [this+0x40]. */
void L_11019780(CPU *c)
{
    push32(c, c->ebx);                                   /* 9780 push ebx     */
    c->edx = 0; flags_logic32(c, 0);                     /* 9781 xor edx,edx  */
    push32(c, c->esi);                                   /* 9783 push esi     */
    push32(c, c->edi);                                   /* 9784 push edi     */
    push32(c, c->ebp);                                   /* 9785 push ebp     */
    c->esi = c->ecx;                                     /* 9786 mov esi,ecx  */
L_9788:
    c->edi = rd32(c->esi + 0x3C);                        /* 9788 mov edi,[esi+3C] */
    c->ecx = rd32(c->esi + 0x40);                        /* 978B mov ecx,[esi+40] */
    c->eax = 0; flags_logic32(c, 0);                     /* 978E xor eax,eax  */
    SET8L(c->ebx, rd8(c->edi));                          /* 9790 mov bl,[edi] */
    SET8L(c->ebx, shr8(c, R8L(c->ebx), R8L(c->ecx)));    /* 9792 shr bl,cl    */
    SET8L(c->ebx, R8L(c->ebx) & 1); flags_logic8(c, R8L(c->ebx)); /* 9794 and bl,1 */
    c->ecx = flags_inc32(c, c->ecx);                     /* 9797 inc ecx      */
    SET8L(c->eax, R8L(c->ebx));                          /* 9798 mov al,bl    */
    wr32(c->esi + 0x40, c->ecx);                         /* 979A mov [esi+40],ecx */
    flags_sub32(c, c->ecx, 7);                           /* 979D cmp ecx,7    */
    if (c->cf || c->zf) goto L_97AD;                     /* 97A0 jbe 97AD     */
    wr32(c->esi + 0x40, 0);                              /* 97A2 mov [esi+40],0 */
    c->edi = flags_inc32(c, c->edi);                     /* 97A9 inc edi      */
    wr32(c->esi + 0x3C, c->edi);                         /* 97AA mov [esi+3C],edi */
L_97AD:
    flags_sub32(c, c->eax, 1);                           /* 97AD cmp eax,1    */
    if (!c->zf) goto L_97B5;                             /* 97B0 jne 97B5     */
    c->edx = flags_inc32(c, c->edx);                     /* 97B2 inc edx      */
    goto L_9788;                                         /* 97B3 jmp 9788     */
L_97B5:
    flags_logic32(c, c->edx & c->edx);                   /* 97B5 test edx,edx */
    if (!c->zf) goto L_97C3;                             /* 97B7 jne 97C3     */
    c->eax = 1;                                          /* 97B9 mov eax,1    */
    c->ebp = pop32(c);                                   /* 97BE pop ebp      */
    c->edi = pop32(c);                                   /* 97BF pop edi      */
    c->esi = pop32(c);                                   /* 97C0 pop esi      */
    c->ebx = pop32(c);                                   /* 97C1 pop ebx      */
    return;                                              /* 97C2 ret          */
L_97C3:
    c->edx = flags_dec32(c, c->edx);                     /* 97C3 dec edx      */
    c->ebx = rd32(c->esi + 0x3C);                        /* 97C4 mov ebx,[esi+3C] */
    c->eax = R16(c->edx);                                /* 97C7 movzx eax,dx */
    c->ecx = rd32(c->esi + 0x40);                        /* 97CA mov ecx,[esi+40] */
    c->ebp = rd32(c->ebx);                               /* 97CD mov ebp,[ebx] */
    c->ebp = shr32(c, c->ebp, R8L(c->ecx));              /* 97CF shr ebp,cl   */
    c->edi = rd32(GVA(0x11020428) + c->eax * 4);         /* 97D1 mov edi,[eax*4+11020428] */
    c->eax = flags_add32(c, c->eax, c->ecx);             /* 97D8 add eax,ecx  */
    c->edi = c->edi & c->ebp; flags_logic32(c, c->edi);  /* 97DA and edi,ebp  */
    c->ecx = c->eax;                                     /* 97DC mov ecx,eax  */
    c->ecx = shr32(c, c->ecx, 3);                        /* 97DE shr ecx,3    */
    wr32(c->esi + 0x40, c->eax);                         /* 97E1 mov [esi+40],eax */
    c->ecx = flags_add32(c, c->ecx, c->ebx);             /* 97E4 add ecx,ebx  */
    c->eax = c->eax & 7; flags_logic32(c, c->eax);       /* 97E6 and eax,7    */
    wr32(c->esi + 0x3C, c->ecx);                         /* 97E9 mov [esi+3C],ecx */
    wr32(c->esi + 0x40, c->eax);                         /* 97EC mov [esi+40],eax */
    c->eax = 1;                                          /* 97EF mov eax,1    */
    SET8L(c->ecx, R8L(c->edx));                          /* 97F4 mov cl,dl    */
    c->eax = shl32(c, c->eax, R8L(c->ecx));              /* 97F6 shl eax,cl   */
    c->eax = c->eax | c->edi; flags_logic32(c, c->eax);  /* 97F8 or eax,edi   */
    c->ebp = pop32(c);                                   /* 97FA pop ebp      */
    c->eax = flags_inc32(c, c->eax);                     /* 97FB inc eax      */
    c->edi = pop32(c);                                   /* 97FC pop edi      */
    c->esi = pop32(c);                                   /* 97FD pop esi      */
    c->ebx = pop32(c);                                   /* 97FE pop ebx      */
    return;                                              /* 97FF ret          */
}
