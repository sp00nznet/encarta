/*
 * recomp_enc97_run.c - BOOT the lifted ENC97.EXE: execute the recompiled entry
 * point (start @0x50DB70) through the 7,326-function dispatch table, with the
 * full 914-import IAT wired to real Win32/MFC/CRT.
 *
 * Internal calls (direct original-VA or indirect relocated live-VA) route to the
 * lifted C function if present, else fall back to the real mapped original.
 * Imports (addresses outside the image) are real DLL functions called through an
 * esp-switch trampoline. The lifted entry runs on a big worker stack under a
 * watchdog; we report how far the recompiled code gets (call count, last lifted
 * function, whether a window appeared) before the watchdog stops it.
 *
 * Build 32-bit. Needs the generated enc97_full.c (the full lift).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "cpu.h"

#define PREF_BASE 0x400000u

#include "enc97_full_list.h"
#define DECL(a) void L_##a(CPU *);
LIFTED_FUNCS(DECL)
#undef DECL

typedef void (*lfn)(CPU *);
typedef struct { uint32_t va; lfn fn; } entry_t;
static entry_t g_lifted[] = {
#define E(a) { 0x##a, L_##a },
    LIFTED_FUNCS(E)
#undef E
};
#define NLIFTED (sizeof g_lifted / sizeof *g_lifted)

static uint8_t *g_base;
static uint32_t g_imgsz;
static volatile unsigned long g_calls;
static volatile uint32_t g_last;        /* last internal target dispatched */
static volatile uint32_t g_last_import; /* last import target */
static volatile int g_done;
static int g_trace;   /* RUN_TRACE: log the first N import calls of the boot */
static uint32_t g_initterm;            /* resolved MSVCRT40!_initterm address */
static unsigned long g_initfns;        /* C++/CRT init fns routed through lifted dispatch */
static unsigned long g_r2l_calls;      /* real->lifted (vtable/fn-ptr) calls */
static int g_heapcheck;                /* R2L_HEAPCHECK: HeapValidate after each r2l call */
/* resolved-address -> "dll!name" for tracing which import the boot is in */
static struct { uint32_t addr; char name[64]; } g_inames[1024];
static int g_ninames;
static const char *imp_name(uint32_t addr)
{
    for (int i = 0; i < g_ninames; i++) if (g_inames[i].addr == addr) return g_inames[i].name;
    return "?";
}

static int cmp_entry(const void *a, const void *b)
{ uint32_t x=((const entry_t*)a)->va, y=((const entry_t*)b)->va; return (x>y)-(x<y); }
static lfn lookup(uint32_t va)
{
    size_t lo=0, hi=NLIFTED;
    while (lo<hi){ size_t m=(lo+hi)/2;
        if (g_lifted[m].va<va) lo=m+1; else if (g_lifted[m].va>va) hi=m; else return g_lifted[m].fn; }
    return NULL;
}

/* esp-switch trampoline: run real code (import or real internal) on the emulated
   stack, loading GP regs from the CPU and capturing eax+esp afterwards. */
static uint32_t T_eax,T_ecx,T_edx,T_ebx,T_esi,T_edi,T_espp4,T_tgt,T_fesp,T_sesp;
static void call_machine(CPU *c, uint32_t target)
{
    T_eax=c->eax; T_ecx=c->ecx; T_edx=c->edx; T_ebx=c->ebx; T_esi=c->esi; T_edi=c->edi;
    T_espp4=c->esp+4; T_tgt=target;
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov T_sesp, esp
        mov eax, T_eax
        mov ecx, T_ecx
        mov edx, T_edx
        mov ebx, T_ebx
        mov esi, T_esi
        mov edi, T_edi
        mov esp, T_espp4
        call dword ptr [T_tgt]
        mov T_fesp, esp
        mov T_eax, eax
        mov esp, T_sesp
        pop ebp
        pop edi
        pop esi
        pop ebx
    }
    c->eax=T_eax; c->esp=T_fesp;
}

static uint8_t *g_tramp_pool; static size_t g_tramp_off;   /* (defined below; fwd for dispatch) */

void dispatch(CPU *c, uint32_t target)
{
    g_calls++;
    /* a redirected vtable/fn-pointer slot may hand us a trampoline address; the
       lifted target's original-VA is the `mov eax, ova` immediate at stub+1. */
    if (g_tramp_pool && target >= (uint32_t)(uintptr_t)g_tramp_pool &&
        target < (uint32_t)(uintptr_t)g_tramp_pool + g_tramp_off) {
        uint32_t ova = *(uint32_t *)(uintptr_t)(target + 1);
        lfn fn = lookup(ova);
        if (fn) { g_last = ova; fn(c); return; }
    }
    uint32_t ova = 0;
    if (target >= PREF_BASE && target < PREF_BASE + g_imgsz)
        ova = target;                                   /* direct internal (original VA) */
    else if (target >= (uint32_t)(uintptr_t)g_base &&
             target <  (uint32_t)(uintptr_t)g_base + g_imgsz)
        ova = target - g_image_delta;                   /* indirect relocated live internal */
    if (ova) {
        g_last = ova;
        lfn fn = lookup(ova);
        if (fn) { fn(c); return; }
        call_machine(c, ova + g_image_delta);           /* unlifted internal -> real original */
        return;
    }
    g_last_import = target;
    /* Intercept _initterm: walk the C++/CRT init-pointer table and route each
       entry through our dispatch so the static initializers run as LIFTED code
       (this is the real->lifted path that real MFC would otherwise call directly). */
    if (target == g_initterm && g_initterm) {
        uint32_t b = rd32(c->esp + 4), e = rd32(c->esp + 8);
        for (uint32_t p = b; p < e; p += 4) {
            uint32_t fn = rd32(p);
            if (fn) { g_initfns++; push32(c, 0xDEADBEEFu); dispatch(c, fn); }
        }
        c->eax = 0; c->esp += 4;     /* mimic cdecl ret; caller cleans the 2 args */
        return;
    }
    const char *nm = imp_name(target);
    if (g_trace && g_calls <= (unsigned)g_trace)
        fprintf(stderr, "  [%lu] import %s\n", g_calls, nm), fflush(stderr);
    /* the app terminating means the lifted entry drove CRT+MFC startup to its
       end; report the milestone cleanly instead of vanishing inside real exit(). */
    if (!strcmp(nm,"MSVCRT40.dll!exit") || !strcmp(nm,"MSVCRT40.dll!_exit") ||
        !strcmp(nm,"KERNEL32.dll!ExitProcess")) {
        uint32_t code = rd32(c->esp + 4);
        fprintf(stderr, "\n*** lifted ENC97 reached %s(%u) after %lu dispatched calls\n", nm, code, g_calls);
        fflush(stderr);
        printf("ENC97 lifted boot: ran CRT+MFC startup to %s(%u) — %lu calls dispatched "
               "(%lu init fns, %lu real->lifted vtable calls), last lifted fn 0x%06X\n",
               nm, code, g_calls, g_initfns, g_r2l_calls, g_last);
        fflush(stdout);
        ExitProcess(code);
    }
    call_machine(c, target);                            /* import -> real DLL */
}
void dispatch_jmp(CPU *c, uint32_t t){ dispatch(c,t); }
void dispatch_indirect(CPU *c, uint32_t t){ dispatch(c,t); }

static uint8_t *g_estack;
#define EMU_STACK (4u << 20)
static uint32_t call_lifted(lfn fn, const uint32_t *args, int n)
{
    CPU c; memset(&c,0,sizeof c);
    c.esp = (uint32_t)(uintptr_t)(g_estack + EMU_STACK - 4096);
    for (int i=n-1;i>=0;i--) push32(&c,args[i]);
    push32(&c, 0xDEADBEEFu);
    fn(&c);
    return c.eax;
}

/* ================= real -> lifted thiscall trampoline =====================
 * The inverse of call_machine: when REAL code (e.g. MFC virtual dispatch) calls
 * a function pointer that we've redirected here, run the LIFTED function instead.
 * A per-target stub (`mov eax, ova; jmp r2l_common`) lands in r2l_common with
 * eax=target original-VA, ecx=this, [esp]=retaddr, args above. r2l_helper copies
 * the args onto a private emulated-stack frame (so the lifted code's pushes can't
 * collide with the real stack), runs the lifted fn, and reports how many arg
 * bytes its `ret N` cleaned so the trampoline returns thiscall-correctly.
 * Reentrant: nesting frames are LIFO on a dedicated arena; result+pop come back
 * in edx:eax (no globals). */
static uint8_t *g_r2l_arena; static uint32_t g_r2l_top;
#define R2L_ARENA (8u<<20)
#define R2L_FRAME 0x8000u

static uint64_t __cdecl r2l_helper(uint32_t ova, uint32_t this_, uint32_t *real_args,
                                   uint32_t ebx, uint32_t esi, uint32_t edi, uint32_t ebp)
{
    uint32_t save = g_r2l_top;
    g_r2l_top -= R2L_FRAME;
    uint32_t argsp = g_r2l_top + R2L_FRAME - 0x100;     /* args near frame top */
    for (int i = 0; i < 16; i++) wr32(argsp + 4 + i*4, real_args[i]);
    wr32(argsp, 0xDEADBEEFu);                            /* fake return slot */
    CPU c; memset(&c, 0, sizeof c);
    /* seed all GP regs from the real caller: some routed targets are thunks that
       use the CALLER's ebp (e.g. `lea ecx,[ebp-0x10]`) without a frame of own. */
    c.ecx = this_; c.ebx = ebx; c.esi = esi; c.edi = edi; c.ebp = ebp; c.esp = argsp;
    g_r2l_calls++;
    dispatch(&c, ova + g_image_delta);                   /* -> lifted */
    if (g_heapcheck) {                                   /* pinpoint the corrupting call */
        HANDLE hs[64]; DWORD nh = GetProcessHeaps(64, hs);
        for (DWORD i = 0; i < nh; i++)
            if (!HeapValidate(hs[i], 0, NULL)) {
                fprintf(stderr, "HEAP CORRUPT after lifted 0x%06X (r2l #%lu, heap %p)\n",
                        ova, g_r2l_calls, hs[i]); fflush(stderr); break;
            }
    }
    uint32_t pop = (c.esp >= argsp + 4) ? (c.esp - argsp - 4) : 0;
    g_r2l_top = save;
    return (uint64_t)c.eax | ((uint64_t)(pop & 0xFFFF) << 32);
}

__declspec(naked) static void r2l_common(void)
{
    __asm {
        push ebp
        mov  ebp, esp                  /* [ebp+4]=retaddr, [ebp+8]=arg0, [ebp]=caller ebp */
        push dword ptr [ebp]           /* caller's ebp (for ebp-relative thunks) */
        push edi
        push esi
        push ebx
        lea  edx, [ebp+8]
        push edx                       /* real_args */
        push ecx                       /* this */
        push eax                       /* ova */
        call r2l_helper                /* edx:eax = pop:result */
        add  esp, 28                   /* clean 7 cdecl args */
        mov  ecx, edx                  /* ecx = pop bytes */
        mov  edx, [ebp+4]              /* retaddr */
        mov  esp, ebp
        pop  ebp
        add  esp, 4                    /* pop retaddr */
        add  esp, ecx                  /* thiscall: callee cleans args */
        jmp  edx                       /* return (eax=result) */
    }
}

static uint32_t make_tramp(uint32_t ova)
{
    uint8_t *s = g_tramp_pool + g_tramp_off; g_tramp_off += 16;
    s[0]=0xB8; *(uint32_t*)(s+1)=ova;                     /* mov eax, ova */
    s[5]=0xE9; *(int32_t*)(s+6)=(int32_t)((uint8_t*)r2l_common-(s+10)); /* jmp r2l_common */
    return (uint32_t)(uintptr_t)s;
}

/* Redirect ENC97's vtable / function-pointer slots to real->lifted trampolines,
   so when real MFC virtual-dispatches into the app it lands in LIFTED code. Only
   slots whose target is a FUNCTION START (lookup succeeds) are rewritten — this
   excludes jump tables (their entries are mid-function labels, not fn starts).
   Scans .rdata + .data (where vtables and fn-ptr tables live). */
static int rewrite_fnptr_slots(void)
{
    PIMAGE_DOS_HEADER dos=(PIMAGE_DOS_HEADER)g_base;
    PIMAGE_NT_HEADERS nt=(PIMAGE_NT_HEADERS)(g_base+dos->e_lfanew);
    PIMAGE_SECTION_HEADER s=IMAGE_FIRST_SECTION(nt);
    uint32_t tlo=0,thi=0; uint32_t base=(uint32_t)(uintptr_t)g_base;
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++)
        if(!memcmp(s[i].Name,".text",5)){ tlo=base+s[i].VirtualAddress;
            thi=tlo+s[i].Misc.VirtualSize; }
    /* A function-start pointer (points into .text at a lifted fn entry). */
    #define IS_FNPTR(val) ((val)>=tlo && (val)<thi && lookup((val)-g_image_delta))
    const int MINVT=3;       /* only rewrite runs of >=3 (real vtables); a run of */
                             /* 3 consecutive valid fn-starts is not coincidence,  */
                             /* so this avoids clobbering data that merely looks    */
                             /* like a pointer. */
    int n=0;
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++){
        if(memcmp(s[i].Name,".rdata",6) && memcmp(s[i].Name,".data",5)) continue;
        uint32_t a=base+s[i].VirtualAddress, e=a+s[i].Misc.VirtualSize;
        for(uint32_t p=a; p+4<=e; ){
            uint32_t v=*(uint32_t*)(uintptr_t)p;
            if(IS_FNPTR(v)){
                uint32_t q=p; int run=0;             /* measure the run length */
                while(q+4<=e && IS_FNPTR(*(uint32_t*)(uintptr_t)q)){ run++; q+=4; }
                if(run>=MINVT)
                    for(uint32_t r=p;r<q;r+=4){
                        uint32_t ova=*(uint32_t*)(uintptr_t)r - g_image_delta;
                        *(uint32_t*)(uintptr_t)r=make_tramp(ova); n++;
                    }
                p=q;
            } else p+=4;
        }
    }
    return n;
}

/* ---- map + relocate ENC97, then wire its full import table to real code ---- */
static HMODULE load_for(const char *dll)
{
    char path[MAX_PATH];
    if (!_stricmp(dll,"DECO_32.DLL")||!_stricmp(dll,"ENCAPI32.dll")||!_stricmp(dll,"EEUIL10.dll")){
        snprintf(path,sizeof path,"C:\\encarta\\analysis\\%s",dll); return LoadLibraryA(path);
    }
    if (!_stricmp(dll,"MSVCRT40.dll")) return LoadLibraryA("msvcrt.dll");
    return LoadLibraryA(dll);
}
static int g_imports, g_imports_res;
static int map_and_wire(const char *path)
{
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *file=malloc(sz); if(fread(file,1,sz,f)!=(size_t)sz){fclose(f);return 0;} fclose(f);
    PIMAGE_DOS_HEADER dos=(PIMAGE_DOS_HEADER)file;
    PIMAGE_NT_HEADERS nt=(PIMAGE_NT_HEADERS)(file+dos->e_lfanew);
    uint32_t imgsz=nt->OptionalHeader.SizeOfImage;
    /* OS-chosen base; relocs are applied below and the lifter now GVA-wraps
       address immediates, so the lifted code is correct at any base. */
    uint8_t *base=VirtualAlloc(NULL,imgsz,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    if(!base){free(file);return 0;}
    memcpy(base,file,nt->OptionalHeader.SizeOfHeaders);
    PIMAGE_SECTION_HEADER s=IMAGE_FIRST_SECTION(nt);
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++)
        if(s[i].SizeOfRawData) memcpy(base+s[i].VirtualAddress,file+s[i].PointerToRawData,s[i].SizeOfRawData);
    g_base=base; g_imgsz=imgsz; g_image_delta=(int32_t)((uint32_t)(uintptr_t)base-PREF_BASE);
    IMAGE_DATA_DIRECTORY rd=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if(g_image_delta && rd.Size){
        uint8_t *p=base+rd.VirtualAddress,*end=p+rd.Size;
        while(p<end){ PIMAGE_BASE_RELOCATION br=(PIMAGE_BASE_RELOCATION)p;
            uint32_t n=(br->SizeOfBlock-sizeof *br)/2; uint16_t *e=(uint16_t*)(br+1);
            for(uint32_t i=0;i<n;i++) if((e[i]>>12)==IMAGE_REL_BASED_HIGHLOW)
                *(uint32_t*)(base+br->VirtualAddress+(e[i]&0xFFF))+=g_image_delta;
            p+=br->SizeOfBlock; }
    }
    /* wire imports to real code (read descriptors from `file` headers, write the
       mapped IAT in `base`); free `file` only after we're done with `nt`. */
    IMAGE_DATA_DIRECTORY id=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    PIMAGE_IMPORT_DESCRIPTOR imp=(PIMAGE_IMPORT_DESCRIPTOR)(base+id.VirtualAddress);
    for(;imp->Name;imp++){
        const char *dll=(const char*)(base+imp->Name);
        HMODULE h=load_for(dll);
        uint32_t oft=imp->OriginalFirstThunk?imp->OriginalFirstThunk:imp->FirstThunk;
        uint32_t *lookup_t=(uint32_t*)(base+oft);
        uint32_t *iat=(uint32_t*)(base+imp->FirstThunk);
        for(int i=0;lookup_t[i];i++){
            uint32_t ent=lookup_t[i]; FARPROC p=NULL;
            if(h){ if(ent&0x80000000u) p=GetProcAddress(h,(LPCSTR)(uintptr_t)(ent&0xFFFF));
                   else p=GetProcAddress(h,(LPCSTR)(base+(ent&0x7FFFFFFF)+2)); }
            g_imports++;
            if(p){ iat[i]=(uint32_t)(uintptr_t)p; g_imports_res++;
                if(g_ninames < 1024){ g_inames[g_ninames].addr=(uint32_t)(uintptr_t)p;
                    if(ent&0x80000000u) snprintf(g_inames[g_ninames].name,64,"%s#%u",dll,ent&0xFFFF);
                    else snprintf(g_inames[g_ninames].name,64,"%s!%s",dll,(const char*)(base+(ent&0x7FFFFFFF)+2));
                    if(!_stricmp(dll,"MSVCRT40.dll") && !(ent&0x80000000u) &&
                       !strcmp((const char*)(base+(ent&0x7FFFFFFF)+2),"_initterm"))
                        g_initterm=(uint32_t)(uintptr_t)p;
                    g_ninames++; }
            }
        }
    }
    free(file);
    return 1;
}

static volatile DWORD g_fault_code;
/* Vectored handler: faults inside real code called on the switched trampoline
   stack are uncatchable by SEH, so log diagnostics here (then continue search). */
static LONG CALLBACK veh_diag(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x40010006 || code == 0x4001000A) return EXCEPTION_CONTINUE_EXECUTION; /* OutputDebugString */
    if (code >= 0xC0000000u) {
        uint32_t fa = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        uint32_t ova = (fa >= (uint32_t)(uintptr_t)g_base && fa < (uint32_t)(uintptr_t)g_base + g_imgsz)
                     ? fa - g_image_delta : 0;
        fprintf(stderr, "VEH fault 0x%08lX @%08X%s after %lu calls; last lifted=0x%06X last import=0x%08X (%s)\n",
                code, fa, ova ? "" : " (external)", g_calls, g_last, g_last_import, imp_name(g_last_import));
        if (ova) fprintf(stderr, "  faulting addr is ENC97 original-VA 0x%06X\n", ova);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct winscan { DWORD pid; char *title; };
static BOOL CALLBACK find_proc_window(HWND h, LPARAM lp)
{
    struct winscan *w = (struct winscan *)lp; DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p == w->pid && IsWindowVisible(h) && GetWindowTextLengthA(h) > 0) {
        GetWindowTextA(h, w->title, 250); return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI run_thread(LPVOID arg)
{
    (void)arg;
    __try {
        call_lifted(L_0050DB70, NULL, 0);   /* lifted CRT/MFC entry: start() */
        g_done = 1;
    } __except (g_fault_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "FAULT 0x%08lX after %lu calls; last lifted fn=0x%06X last import=0x%08X (%s)\n",
                g_fault_code, g_calls, g_last, g_last_import, imp_name(g_last_import));
        fflush(stderr);
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    { const char *t = getenv("RUN_TRACE"); g_trace = t ? atoi(t) : 0; }
    g_heapcheck = getenv("R2L_HEAPCHECK") != NULL;
    AddVectoredExceptionHandler(1, veh_diag);
    fprintf(stderr,"[1] start\n");
    const char *exe = (argc>=2)?argv[1]:"C:\\encarta\\analysis\\ENC97.EXE";
    int timeout_ms = (argc>=3)?atoi(argv[2]):8000;
    if(!map_and_wire(exe)){ fprintf(stderr,"map/wire failed\n"); return 1; }
    fprintf(stderr,"[2] mapped+wired\n");
    qsort(g_lifted,NLIFTED,sizeof *g_lifted,cmp_entry);
    g_estack=VirtualAlloc(NULL,EMU_STACK,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g_r2l_arena=VirtualAlloc(NULL,R2L_ARENA,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g_r2l_top=(uint32_t)(uintptr_t)(g_r2l_arena+R2L_ARENA);
    g_tramp_pool=VirtualAlloc(NULL,0x200000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);

    if(getenv("R2L_TEST")){   /* unit-test the real->lifted thiscall trampoline */
        uint8_t *buf=calloc(0x200,1);
        uint32_t t=make_tramp(0x4AD870u);   /* lifted __thiscall(this,v): *(this+0x8E)=v */
        FlushInstructionCache(GetCurrentProcess(),NULL,0);
        uint32_t this_=(uint32_t)(uintptr_t)buf, val=0xCAFEF00Du, tgt=t;
        __asm {                              /* real thiscall call: ecx=this, arg on stack */
            mov  ecx, this_
            push val
            mov  eax, tgt
            call eax                         /* trampoline does thiscall ret 4 cleanup */
        }
        uint32_t got=*(uint32_t*)(buf+0x8E);
        printf("%s real->lifted thiscall trampoline: sub_4AD870(this,0xCAFEF00D) via REAL call -> this+0x8E=%08X\n",
               got==0xCAFEF00Du?"PASS":"FAIL",got);
        return got==0xCAFEF00Du?0:1;
    }
    fprintf(stderr,"ENC97 mapped @%p (delta %+d); IAT %d/%d wired; %u lifted fns\n",
            (void*)g_base,(int)g_image_delta,g_imports_res,g_imports,(unsigned)NLIFTED);
    if(getenv("R2L_VTABLES")){
        int nv=rewrite_fnptr_slots();
        FlushInstructionCache(GetCurrentProcess(),NULL,0);
        fprintf(stderr,"routed %d vtable/fn-ptr slots -> real->lifted trampolines\n",nv);
    }
    fprintf(stderr,"booting lifted entry start@0x50DB70 (watchdog %d ms)...\n",timeout_ms);

    /* run the lifted entry on a worker with a big stack (lifted calls recurse on
       the real C stack); watchdog + window poll from the main thread. */
    HANDLE th=CreateThread(NULL, 64u<<20, run_thread, NULL, 0, NULL);
    char wintitle[256] = {0};
    int elapsed = 0, step = 100;
    while (elapsed < timeout_ms) {
        if (WaitForSingleObject(th, step) == WAIT_OBJECT_0) break;
        elapsed += step;
        /* did the booting app create a top-level window in our process? */
        struct winscan wp = { GetCurrentProcessId(), wintitle };
        EnumWindows(find_proc_window, (LPARAM)&wp);
        if (wintitle[0]) break;
    }

    if(g_done)
        fprintf(stderr,"lifted entry RETURNED after %lu dispatched calls\n", g_calls);
    else
        fprintf(stderr,"%s: %lu calls; last lifted fn=0x%06X last import=0x%08X (%s)\n",
                wintitle[0]?"window appeared":"watchdog stop",
                g_calls, g_last, g_last_import, imp_name(g_last_import));

    printf("ENC97 lifted boot: %lu calls dispatched, last lifted fn 0x%06X%s%s%s\n",
           g_calls, g_last,
           g_done?" (entry returned)":" (running)",
           wintitle[0]?" — window: " : "", wintitle);
    TerminateThread(th, 0);   /* bounded: stop whatever the boot is doing */
    return 0;
}
