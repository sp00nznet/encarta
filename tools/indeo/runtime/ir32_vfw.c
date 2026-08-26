/*
 * ir32_vfw.c - present the recompiled Indeo 3 codec to Video for Windows.
 *
 * The decoder is exact but nothing could reach it: Encarta opens its clips
 * through MCI, the MCIAVI driver asks VFW for a decompressor for `IV32`, and
 * VFW looks for one among the installed video codecs. There is no such codec on
 * a modern Windows - Microsoft removed the Indeo family - and the one on the CD
 * is a 16-bit NE that a 32-bit process cannot load. That is the whole reason
 * the DLL was recompiled in the first place.
 *
 * ICInstall closes the gap without hooking anything. With ICINSTALL_FUNCTION it
 * registers a DriverProc for THIS PROCESS only, and every ICLocate afterwards -
 * including the one MCIAVI performs, since it runs in the same process - finds
 * it. That is the documented mechanism for exactly this case, so the bridge is
 * a driver rather than an interception.
 *
 * What this file does is marshalling. VFW speaks 32-bit pointers; the lifted
 * codec speaks 16-bit far pointers into the arena. So each message copies the
 * caller's structures into arena objects, calls the lifted DriverProc, and
 * copies the result back. The messages themselves pass straight through - the
 * recompiled driver already answers DRV_LOAD, ICM_DECOMPRESS_BEGIN and the
 * rest, because they are its own.
 *
 * Not thread-safe and single-stream: the codec is one lifted DLL with one
 * arena, so there is one instance. Encarta plays one clip at a time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <vfw.h>
#include "ne_mem.h"

uint32_t ir32_driver_call(uint32_t driver_id, uint16_t hdrv, uint16_t msg,
                          uint32_t lp1, uint32_t lp2,
                          uint16_t ds, uint16_t ss, uint16_t sp);
void ir32_register16(void);
void ir32_register32(void);
uint16_t ne_init(uint32_t stack_bytes);
int ir32_load_ne(const char *path);
extern uint16_t g_ne_autodata;

/* Selectors for the objects handed to the codec. Deliberately clear of the
 * ones ir32_run.c uses for its own decode path, so the two can coexist in a
 * build without silently sharing arena objects. */
enum {
    V_BIIN = 0x0320, V_BIOUT, V_IN, V_OUT, V_ICD, V_ICOPEN, V_BIFMT
};

static struct {
    int         loaded;      /* the NE is in the arena and registered */
    int         opened;      /* DRV_OPEN succeeded */
    uint32_t    id;          /* driver instance the codec handed back */
    uint16_t    ds, ss;
    uint32_t    in_cap, out_cap;
    int         begun;
    int32_t     w, h;
    uint16_t    bits;
} g;

static uint32_t farptr(uint16_t sel) { return ((uint32_t)sel << 16); }
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

/* Grow an arena object if the frame needs more than last time.
 *
 * ne_alloc always takes fresh arena space, so calling it per frame would walk
 * through 32 MB and abort partway into a clip. Allocating once and only when
 * the size actually rises keeps a long playback to a handful of allocations. */
static unsigned char *ensure(uint16_t sel, uint32_t need, uint32_t *cap)
{
    if (!*cap || need > *cap) {
        uint32_t want = need + (need >> 2) + 0x10000u;
        ne_alloc(sel, NULL, 0, want);
        *cap = want;
    }
    return g_arena + g_segoff[sel];
}

/* Copy a BITMAPINFOHEADER (and its colour table, if any) into the arena. */
static void put_bi(uint16_t sel, const BITMAPINFOHEADER *bi)
{
    unsigned char *p = g_arena + g_segoff[sel];
    uint32_t n = bi->biSize ? bi->biSize : sizeof(BITMAPINFOHEADER);
    if (n > 40) n = 40;
    memcpy(p, bi, n);
    /* A palettised format carries 256 RGBQUADs after the header and the codec
     * both reads and fills them, so the object has to have room for them
     * whether or not the caller supplied any. */
    if (bi->biBitCount <= 8) {
        uint32_t used = bi->biClrUsed ? bi->biClrUsed : 256u;
        if (used > 256) used = 256;
        memcpy(p + 40, (const unsigned char *)bi + n, used * 4);
    }
}

static LRESULT decompress_query(ICDECOMPRESS *icd, const BITMAPINFOHEADER *in,
                                const BITMAPINFOHEADER *out)
{
    (void)icd;
    if (!in || in->biCompression != mmioFOURCC('I', 'V', '3', '2'))
        return ICERR_BADFORMAT;
    if (!out)
        return ICERR_OK;              /* "can you read this input at all" */
    if (out->biCompression != BI_RGB)
        return ICERR_BADFORMAT;
    if (out->biWidth != in->biWidth || abs(out->biHeight) != abs(in->biHeight))
        return ICERR_BADFORMAT;
    /* 8, 16 and 24 are what the codec accepts; it answers ICERR_BADFORMAT for
     * 1, 4 and 32 itself, and asking it costs a full message round trip. */
    if (out->biBitCount != 8 && out->biBitCount != 16 && out->biBitCount != 24)
        return ICERR_BADFORMAT;
    return ICERR_OK;
}

static LRESULT get_format(const BITMAPINFOHEADER *in, BITMAPINFOHEADER *out)
{
    if (!in)
        return ICERR_BADFORMAT;
    if (!out)
        return sizeof(BITMAPINFOHEADER);   /* size query */
    put_bi(V_BIIN, in);
    ne_alloc(V_BIFMT, NULL, 0, 40 + 256 * 4);
    uint32_t r = ir32_driver_call(g.id, 1, 0x400A,
                                  farptr(V_BIIN), farptr(V_BIFMT),
                                  g.ds, g.ss, 0xFF00);
    if ((int32_t)r != ICERR_OK)
        return (LRESULT)(int32_t)r;
    memcpy(out, g_arena + g_segoff[V_BIFMT], sizeof(BITMAPINFOHEADER));
    return ICERR_OK;
}

static LRESULT decompress_begin(const BITMAPINFOHEADER *in,
                                const BITMAPINFOHEADER *out)
{
    LRESULT q = decompress_query(NULL, in, out);
    if (q != ICERR_OK)
        return q;
    put_bi(V_BIIN, in);
    put_bi(V_BIOUT, out);
    uint32_t r = ir32_driver_call(g.id, 1, 0x400C,
                                  farptr(V_BIIN), farptr(V_BIOUT),
                                  g.ds, g.ss, 0xFF00);
    if ((int32_t)r == ICERR_OK) {
        g.begun = 1;
        g.w = in->biWidth;
        g.h = abs(in->biHeight);
        g.bits = out->biBitCount;
    }
    return (LRESULT)(int32_t)r;
}

static LRESULT decompress(ICDECOMPRESS *icd, DWORD cb)
{
    if (!icd || !icd->lpbiInput || !icd->lpInput || !icd->lpOutput)
        return ICERR_BADPARAM;
    if (!g.begun) {
        LRESULT b = decompress_begin(icd->lpbiInput, icd->lpbiOutput);
        if (b != ICERR_OK)
            return b;
    }
    const BITMAPINFOHEADER *bi = icd->lpbiInput;
    uint32_t insz = bi->biSizeImage;
    if (!insz)
        return ICERR_BADPARAM;

    /* The output is sized from the format the caller asked for, with slack:
     * the codec's row pitch is its own decision and it writes past a
     * tightly-sized DIB - see the 12,032 bytes it put past a 216x192 8bpp
     * buffer before this was understood. */
    const BITMAPINFOHEADER *bo = icd->lpbiOutput;
    uint32_t outsz = bo->biSizeImage;
    if (!outsz)
        outsz = (uint32_t)bo->biWidth * abs(bo->biHeight) *
                ((bo->biBitCount + 7) / 8);

    unsigned char *ip = ensure(V_IN, insz, &g.in_cap);
    ensure(V_OUT, outsz + 0x10000u, &g.out_cap);
    memcpy(ip, icd->lpInput, insz);
    put_bi(V_BIIN, bi);
    put_bi(V_BIOUT, bo);

    ne_alloc(V_ICD, NULL, 0, 64);
    unsigned char *p = g_arena + g_segoff[V_ICD];
    memset(p, 0, 64);
    put32(p +  0, icd->dwFlags);
    put32(p +  4, farptr(V_BIIN));
    put32(p +  8, farptr(V_IN));
    put32(p + 12, farptr(V_BIOUT));
    put32(p + 16, farptr(V_OUT));
    put32(p + 20, icd->ckid);

    uint32_t r = ir32_driver_call(g.id, 1, 0x400D, farptr(V_ICD), 24,
                                  g.ds, g.ss, 0xFF00);
    if ((int32_t)r != ICERR_OK)
        return (LRESULT)(int32_t)r;
    memcpy(icd->lpOutput, g_arena + g_segoff[V_OUT], outsz);
    (void)cb;
    return ICERR_OK;
}

static LRESULT getinfo(ICINFO *ii, DWORD cb)
{
    if (!ii)
        return sizeof(ICINFO);
    if (cb < sizeof(ICINFO))
        return 0;
    memset(ii, 0, sizeof *ii);
    ii->dwSize = sizeof(ICINFO);
    ii->fccType = ICTYPE_VIDEO;
    ii->fccHandler = mmioFOURCC('I', 'V', '3', '2');
    ii->dwFlags = VIDCF_TEMPORAL;
    ii->dwVersion = 0x00030200;
    ii->dwVersionICM = ICVERSION;
    wcscpy(ii->szName, L"IV32");
    wcscpy(ii->szDescription, L"Indeo 3.2 (statically recompiled IR32.DLL)");
    return sizeof(ICINFO);
}

LRESULT CALLBACK ir32_DriverProc(DWORD_PTR dwDriverID, HDRVR hDriver,
                                 UINT msg, LPARAM lp1, LPARAM lp2)
{
    (void)hDriver;
    switch (msg) {
    case DRV_LOAD:      return 1;
    case DRV_FREE:      return 1;
    case DRV_ENABLE:    return 1;
    case DRV_DISABLE:   return 1;
    case DRV_INSTALL:   return DRVCNF_OK;
    case DRV_REMOVE:    return DRVCNF_OK;
    case DRV_QUERYCONFIGURE: return 0;

    case DRV_OPEN: {
        /* VFW opens a driver with lp2 pointing at an ICOPEN when it wants a
         * codec instance, and with NULL when it is only enumerating. Answering
         * the enumeration with a live instance is what makes ICLocate hand out
         * a handle it never closes. */
        ICOPEN *ico = (ICOPEN *)lp2;
        if (ico && ico->fccType != ICTYPE_VIDEO)
            return 0;
        if (!g.loaded)
            return 0;
        return (LRESULT)(dwDriverID ? dwDriverID : 1);
    }
    case DRV_CLOSE:
        g.begun = 0;
        return 1;

    case ICM_GETINFO:
        return getinfo((ICINFO *)lp1, (DWORD)lp2);
    case ICM_ABOUT:
        return ICERR_UNSUPPORTED;
    case ICM_GETSTATE:
    case ICM_SETSTATE:
        return 0;

    case ICM_DECOMPRESS_QUERY:
        return decompress_query(NULL, (const BITMAPINFOHEADER *)lp1,
                                (const BITMAPINFOHEADER *)lp2);
    case ICM_DECOMPRESS_GET_FORMAT:
        return get_format((const BITMAPINFOHEADER *)lp1,
                          (BITMAPINFOHEADER *)lp2);
    case ICM_DECOMPRESS_BEGIN:
        return decompress_begin((const BITMAPINFOHEADER *)lp1,
                                (const BITMAPINFOHEADER *)lp2);
    case ICM_DECOMPRESS:
        return decompress((ICDECOMPRESS *)lp1, (DWORD)lp2);
    case ICM_DECOMPRESS_END:
        g.begun = 0;
        return ICERR_OK;
    case ICM_DECOMPRESS_GET_PALETTE:
        return ICERR_UNSUPPORTED;

    /* ICM_DECOMPRESSEX is the newer entry point and carries its own source and
     * destination rectangles. Declining it makes VFW fall back to
     * ICM_DECOMPRESS, which is the path this codec actually implements. */
    case ICM_DECOMPRESSEX_QUERY:
    case ICM_DECOMPRESSEX_BEGIN:
    case ICM_DECOMPRESSEX:
    case ICM_DECOMPRESSEX_END:
        return ICERR_UNSUPPORTED;

    default:
        if (msg >= DRV_USER)
            return ICERR_UNSUPPORTED;
        return 0;
    }
}

/* Load the recompiled codec and register it for this process.
 *
 * Returns 0 on success. After this, ICLocate/ICOpen for 'IV32' resolve here -
 * including the ones MCIAVI makes on behalf of an application that only ever
 * calls mciSendCommand and knows nothing about any of this.
 */
int ir32_vfw_install(const char *ne_path)
{
    if (g.loaded)
        return 0;
    if (!ir32_load_ne(ne_path))
        return 1;
    ir32_register16();
    ir32_register32();
    g.ds = g_ne_autodata ? g_ne_autodata : 41;
    g.ss = ne_init(256 * 1024);
    g.loaded = 1;

    /* The driver's own load sequence, before VFW sees it. DRV_OPEN returns the
     * instance the codec wants quoted back as dwDriverID on every later
     * message, so it is kept rather than discarded. */
    ir32_driver_call(0, 1, DRV_LOAD, 0, 0, g.ds, g.ss, 0xFF00);
    ir32_driver_call(0, 1, DRV_ENABLE, 0, 0, g.ds, g.ss, 0xFF00);
    ne_alloc(V_ICOPEN, NULL, 0, 64);
    unsigned char *ico = g_arena + g_segoff[V_ICOPEN];
    memset(ico, 0, 64);
    put32(ico + 0, 0);
    memcpy(ico + 4, "vidc", 4);
    memcpy(ico + 8, "IV32", 4);
    put32(ico + 16, ICMODE_DECOMPRESS);
    g.id = ir32_driver_call(0, 1, DRV_OPEN, 0, farptr(V_ICOPEN),
                            g.ds, g.ss, 0xFF00);
    g.opened = 1;

    ne_alloc(V_BIIN, NULL, 0, 40 + 256 * 4);
    ne_alloc(V_BIOUT, NULL, 0, 40 + 256 * 4);

    if (!ICInstall(ICTYPE_VIDEO, mmioFOURCC('I', 'V', '3', '2'),
                   (LPARAM)ir32_DriverProc, NULL, ICINSTALL_FUNCTION)) {
        fprintf(stderr, "ir32_vfw: ICInstall failed\n");
        return 2;
    }
    return 0;
}

void ir32_vfw_remove(void)
{
    if (g.loaded)
        ICRemove(ICTYPE_VIDEO, mmioFOURCC('I', 'V', '3', '2'), 0);
}
