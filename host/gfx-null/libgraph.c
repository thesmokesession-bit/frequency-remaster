#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <eekernel.h>
#include <libgraph.h>

#include "gfxnull.h"

// The Sony libgraph entry points of the native build. The descriptor builders are those of
// sce/ee/src/libgraph.c. The routines that reach the hardware send their transfers through
// HostGfxDma() and wait for blanks through HostWaitVblank().

// The privileged display registers.
#define GS_PMODE (*(volatile unsigned long long *)(uintptr_t)0x12000000U)
#define GS_SMODE2 (*(volatile unsigned long long *)(uintptr_t)0x12000020U)
#define GS_DISPFB1 (*(volatile unsigned long long *)(uintptr_t)0x12000070U)
#define GS_DISPLAY1 (*(volatile unsigned long long *)(uintptr_t)0x12000080U)
#define GS_DISPFB2 (*(volatile unsigned long long *)(uintptr_t)0x12000090U)
#define GS_DISPLAY2 (*(volatile unsigned long long *)(uintptr_t)0x120000A0U)
#define GS_EXTDATA (*(volatile unsigned long long *)(uintptr_t)0x120000C0U)
#define GS_BGCOLOR (*(volatile unsigned long long *)(uintptr_t)0x120000E0U)
#define GS_IMR (*(volatile unsigned long long *)(uintptr_t)0x12001010U)

// The GIF channel registers.
#define GIF_CHANNEL 2
#define GIF_MADR (*(volatile unsigned int *)(uintptr_t)0x1000A010U)
#define GIF_QWC (*(volatile unsigned int *)(uintptr_t)0x1000A020U)

// The shared graphics state block, as libgraph keeps it.
typedef struct {
    short interlaceMode;
    short outputMode;
    short fieldMode;
    short gsVersion;
    int (*vblankHandler)(int);
    int handlerId;
} GsState;

static GsState g_GsStateBlock = {1, 2, 1, 3, NULL, 0};

// The display mode SetGsCrt() last set, for a renderer to read.
static short g_nCrtInterlace;
static short g_nCrtMode;
static short g_nCrtFieldMode;

enum { kGsFieldMode = 0, kGsInterlace = 1, kGsFrameMode = 1, kGsNtsc = 2, kGsPal = 3 };

static GsState *sceGsGetGParam(void) {
    return &g_GsStateBlock;
}

void SetGsCrt(short nInterlace, short nMode, short nFieldMode) {
    g_nCrtInterlace = nInterlace;
    g_nCrtMode = nMode;
    g_nCrtFieldMode = nFieldMode;
}

unsigned long long GsGetIMR(void) {
    return GS_IMR;
}

unsigned long long GsPutIMR(unsigned long long nImr) {
    const unsigned long long nPrevious = GS_IMR;
    GS_IMR = nImr;
    return nPrevious;
}

// Sends nQuadwords from pSource to the GIF as one normal transfer, and finishes it.
static void SendToGif(const void *pSource, unsigned int nQuadwords) {
    const unsigned int address = (unsigned int)(uintptr_t)pSource;

    GIF_QWC = nQuadwords;
    if ((address & 0x70000000U) == 0x70000000U) {
        GIF_MADR = (address & 0x0FFFFFFFU) | 0x80000000U;
    } else {
        GIF_MADR = address & 0x0FFFFFFFU;
    }
    HostGfxDma(GIF_CHANNEL, GIF_MADR, GIF_QWC, 0U, 0x101U);
    GIF_QWC = 0U;
}

void VSync(void) {
    (void)HostWaitVblank();
}

static int sceGszbufaddr(short nPsm, short nWidth, short nHeight) {
    GsState *state = sceGsGetGParam();
    int columns = (nWidth + 63) / 64;
    int rows;
    int blocks;

    if ((nPsm & 2) != 0) {
        rows = (nHeight + 63) / 64;
    } else {
        rows = (nHeight + 31) / 32;
    }
    blocks = columns * rows;
    if (state->interlaceMode == kGsInterlace && state->fieldMode == kGsFieldMode) {
        return (short)blocks;
    }
    return (short)(blocks << 1);
}

static void sceGsPutDispEnv(const sceGsDispEnv *pDisp) {
    GsState *state = sceGsGetGParam();

    if (state->gsVersion == 1) {
        GS_PMODE = pDisp->pmode;
        GS_DISPFB1 = pDisp->dispfb;
        GS_DISPLAY1 = pDisp->display;
        GS_EXTDATA = pDisp->bgcolor;
    } else {
        GS_PMODE = pDisp->pmode;
        GS_SMODE2 = pDisp->smode2;
        GS_DISPFB2 = pDisp->dispfb;
        GS_DISPLAY2 = pDisp->display;
        GS_BGCOLOR = pDisp->bgcolor;
    }
}

// ponytail: VIF1 and the GIF have nothing to reset while transfers finish when they start.
void sceGsResetPath(void) {
}

void sceGsResetGraph(short nMode, short nInterlace, short nOutputMode, short nFieldMode) {
    GsState *state = sceGsGetGParam();

    if (nMode == 0) {
        state->interlaceMode = nInterlace;
        state->outputMode = nOutputMode;
        state->gsVersion = (short)((*HOST_GS_CSR >> 16) & 0xFFULL);
        GsPutIMR(0xFF00ULL);
        state->fieldMode = (short)(nFieldMode != 0);
        if (state->vblankHandler != NULL) {
            DisableIntc(INTC_VBLANK_S);
            RemoveIntcHandler(INTC_VBLANK_S, state->handlerId);
            state->handlerId = 0;
            state->vblankHandler = NULL;
        }
    } else if (nMode == 5) {
        state->fieldMode = (short)(nFieldMode != 0);
        state->interlaceMode = nInterlace;
        state->outputMode = nOutputMode;
        state->gsVersion = (short)((*HOST_GS_CSR >> 16) & 0xFFULL);
    } else {
        return;
    }
    SetGsCrt((short)(nInterlace & 1), (short)(nOutputMode & 0xFF), (short)(nFieldMode & 1));
}

int sceGsSyncV(int nMode) {
    GsState *state = sceGsGetGParam();
    const int field = HostWaitVblank();

    (void)nMode;
    if (state->interlaceMode != 1) {
        return 1;
    }
    return field;
}

int sceGsSetDefAlphaEnv(sceGsAlphaEnv *pAlpha, short nPabe) {
    pAlpha->mWords[1] = 0x42ULL;
    pAlpha->mWords[0] = 0x44ULL;
    pAlpha->mWords[3] = 0x49ULL;
    pAlpha->mWords[2] = (unsigned long long)(long long)nPabe;
    pAlpha->mWords[5] = 0x3BULL;
    pAlpha->mWords[4] = (0x81ULL << 32) | 0x807FULL;
    pAlpha->mWords[7] = 0x4AULL;
    pAlpha->mWords[6] = 0ULL;
    return 4;
}

static unsigned long long MakeDisplayWord(const GsState *state,
                                          short nWidth,
                                          short nHeight,
                                          short nDx,
                                          short nDy,
                                          int nOffsetX,
                                          int nOffsetYInterlaced,
                                          int nOffsetYProgressive) {
    int factor = (nWidth + 0x9FF) / nWidth;
    unsigned long long across = (unsigned long long)((nDx * factor + nOffsetX) & 0xFFF);
    unsigned long long magnify = (unsigned long long)(factor - 1) << 23;
    unsigned long long width = (unsigned long long)(long long)(factor * nWidth - 1) << 32;
    unsigned long long down;
    unsigned long long lines;

    if (state->interlaceMode == kGsInterlace) {
        down = (unsigned long long)((nDy + nOffsetYInterlaced) & 0xFFF) << 12;
        lines =
            (unsigned long long)(long long)(state->fieldMode == 0 ? nHeight - 1 : nHeight * 2 - 1);
    } else {
        down = (unsigned long long)((nDy + nOffsetYProgressive) & 0xFFF) << 12;
        lines = (unsigned long long)(long long)(nHeight - 1);
    }
    return across | down | magnify | width | (lines << 44);
}

void sceGsSetDefDispEnv(
    sceGsDispEnv *pDisp, short nPsm, short nWidth, short nHeight, short nDx, short nDy) {
    GsState *state = sceGsGetGParam();
    long long width = nWidth;
    int interlace = state->interlaceMode;
    int output = state->outputMode;
    int field = state->fieldMode;
    unsigned long long buffer;

    pDisp->pmode = 0x66ULL;
    if (interlace == 0) {
        pDisp->smode2 = 2ULL;
    } else if (field != 0) {
        pDisp->smode2 = 3ULL;
    } else {
        pDisp->smode2 = 1ULL;
    }
    buffer = ((unsigned long long)(nPsm & 0xF) << 15) |
             ((((unsigned long long)(width + 0x3F) >> 6) & 0x3FULL) << 9);
    pDisp->dispfb = buffer;
    if (output == kGsNtsc) {
        pDisp->display = MakeDisplayWord(state, nWidth, nHeight, nDx, nDy, 0x27C, 0x32, 0x19);
    } else if (output == kGsPal) {
        pDisp->display = MakeDisplayWord(state, nWidth, nHeight, nDx, nDy, 0x290, 0x48, 0x24);
    } else {
        printf("sceGsDefDispEnv:Not support displaymode for %d!!\n", output);
    }
    pDisp->bgcolor = 0ULL;
}

int sceGsSetDefDrawEnv(
    sceGsDrawEnv1 *pDraw, short nPsm, short nWidth, short nHeight, short nZTest, short nZPsm) {
    unsigned long long frame;
    unsigned long long depth;
    unsigned long long offset;
    unsigned long long clip;
    unsigned long long test;

    frame = (((unsigned long long)(nPsm & 0xF)) << 24) |
            ((((unsigned long long)(nWidth + 0x3F) >> 6) & 0x3FULL) << 16);
    pDraw->frame1 = frame;
    pDraw->frame1addr = 0x4CULL;
    pDraw->zbuf1addr = 0x4EULL;
    depth = (unsigned long long)sceGszbufaddr(nPsm, nWidth, nHeight);
    depth |= ((unsigned long long)(nZPsm & 0xF)) << 24;
    if (nZTest == 0) {
        depth |= 0x8000ULL << 17;
    }
    pDraw->zbuf1 = depth;
    offset = (((unsigned long long)(0x800 - (nWidth >> 1))) << 4) |
             (((unsigned long long)(0x800 - (nHeight >> 1))) << 36);
    pDraw->xyoffset1 = offset;
    pDraw->xyoffset1addr = 0x18ULL;
    clip = (((unsigned long long)(nWidth - 1)) << 16) | (((unsigned long long)(nHeight - 1)) << 48);
    pDraw->scissor1 = clip;
    pDraw->scissor1addr = 0x40ULL;
    pDraw->prmodecontaddr = 0x1AULL;
    pDraw->prmodecont |= 1ULL;
    pDraw->colclampaddr = 0x46ULL;
    pDraw->colclamp |= 1ULL;
    pDraw->dtheaddr = 0x45ULL;
    if ((nPsm & 2) != 0) {
        pDraw->dthe |= 1ULL;
    } else {
        pDraw->dthe &= ~1ULL;
    }
    if (nZTest == 0) {
        test = 0x30000ULL;
    } else {
        test = (((unsigned long long)(nZTest & 3)) << 17) | 0x10000ULL;
    }
    pDraw->test1 = test;
    pDraw->test1addr = 0x47ULL;
    return 8;
}

int sceGsSetDefClear(sceGsClear *pClear,
                     short nZTest,
                     short nX,
                     short nY,
                     short nWidth,
                     short nHeight,
                     unsigned long long nRed,
                     unsigned long long nGreen,
                     unsigned long long nBlue,
                     unsigned long long nAlpha,
                     unsigned int nZ) {
    unsigned long long first;
    unsigned long long second;
    unsigned long long colour;
    unsigned long long test;

    first = ((unsigned long long)(nX << 4)) | (((unsigned long long)(nY << 4)) << 16);
    second = ((unsigned long long)((nX + nWidth) << 4)) |
             ((unsigned long long)((nY + nHeight) << 4) << 16);
    first |= ((unsigned long long)nZ) << 32;
    second |= ((unsigned long long)nZ) << 32;
    colour = (nRed & 0xFFULL) | ((nGreen & 0xFFULL) << 8) | ((nBlue & 0xFFULL) << 16);
    colour |= (nAlpha & 0xFFULL) << 24;
    colour |= 0xFE00ULL << 46;
    pClear->prim = 6ULL;
    pClear->rgbaqaddr = 1ULL;
    pClear->rgbaqWord = colour;
    pClear->xyz2a = first;
    pClear->xyz2aaddr = 5ULL;
    pClear->xyz2b = second;
    pClear->xyz2baddr = 5ULL;
    pClear->testaaddr = 0x47ULL;
    pClear->testa = 0x30000ULL;
    pClear->primaddr = 0ULL;
    pClear->testbaddr = 0x47ULL;
    if (nZTest == 0) {
        test = 0x30000ULL;
    } else {
        test = (((unsigned long long)(nZTest & 3)) << 17) | 0x10000ULL;
    }
    pClear->testb = test;
    return 6;
}

int sceGsSetDefDBuff(sceGsDBuff *pDBuff,
                     short nPsm,
                     short nWidth,
                     short nHeight,
                     short nZTest,
                     short nZPsm,
                     short nClear) {
    GsState *state = sceGsGetGParam();
    unsigned long long loops = (nClear != 0) ? 0xEULL : 8ULL;
    int clearX = 0x800 - (nWidth >> 1);
    int clearY = 0x800 - (nHeight >> 1);
    int pages;

    sceGsSetDefDispEnv(&pDBuff->disp[0], nPsm, nWidth, nHeight, 0, 0);
    sceGsSetDefDispEnv(&pDBuff->disp[1], nPsm, nWidth, nHeight, 0, 0);
    sceGsSetDefDrawEnv(&pDBuff->draw0, nPsm, nWidth, nHeight, nZTest, nZPsm);
    sceGsSetDefDrawEnv(&pDBuff->draw1, nPsm, nWidth, nHeight, nZTest, nZPsm);
    if (nClear != 0) {
        sceGsSetDefClear(&pDBuff->clear0,
                         nZTest,
                         (short)clearX,
                         (short)clearY,
                         nWidth,
                         nHeight,
                         0ULL,
                         0ULL,
                         0ULL,
                         0ULL,
                         0U);
        sceGsSetDefClear(&pDBuff->clear1,
                         nZTest,
                         (short)clearX,
                         (short)clearY,
                         nWidth,
                         nHeight,
                         0ULL,
                         0ULL,
                         0ULL,
                         0ULL,
                         0U);
    }
    pDBuff->giftag0.mWords[0] = loops | 0x8000ULL | 0x1000000000000000ULL;
    pDBuff->giftag0.mWords[1] = 0xEULL;
    pDBuff->giftag1.mWords[0] = loops | 0x8000ULL | 0x1000000000000000ULL;
    pDBuff->giftag1.mWords[1] = 0xEULL;
    pages = sceGszbufaddr(nPsm, nWidth, nHeight);
    if (!(state->interlaceMode == kGsInterlace && state->fieldMode == kGsFrameMode) &&
        state->interlaceMode != 0) {
        return state->interlaceMode;
    }
    pages >>= 1;
    pDBuff->disp[1].dispfb =
        (pDBuff->disp[1].dispfb & ~0x1FFULL) | (unsigned long long)(pages & 0x1FF);
    pDBuff->draw0.frame1 = (pDBuff->draw0.frame1 & ~0x1FFULL) | (unsigned long long)(pages & 0x1FF);
    return pages & 0x1FF;
}

int sceGsPutDrawEnv(sceGifTag *pGifTag) {
    SendToGif(pGifTag, (unsigned int)(pGifTag->mWords[0] & 0x7FFFULL) + 1U);
    return 0;
}

int sceGsSetDefLoadImage(sceGsLoadImage *pLoadImage,
                         short nTbp,
                         short nTbw,
                         short nPsm,
                         short nSsx,
                         short nSsy,
                         short nRrw,
                         short nRrh) {
    int count = 0;
    unsigned long long buffer;
    unsigned long long position;
    unsigned long long size;

    if (nPsm < 0x3B) {
        switch (nPsm) {
        case 0x00:
        case 0x30:
            count = (nRrw * nRrh) >> 2;
            break;
        case 0x01:
        case 0x31:
            count = ((nRrw * nRrh) << 1) + (nRrw * nRrh);
            count >>= 4;
            break;
        case 0x02:
        case 0x0A:
        case 0x32:
        case 0x3A:
            count = (nRrw * nRrh) >> 3;
            break;
        case 0x13:
        case 0x1B:
            count = (nRrw * nRrh) >> 4;
            break;
        case 0x14:
        case 0x24:
        case 0x2C:
            count = (nRrw * nRrh) >> 5;
            break;
        default:
            break;
        }
    }
    if (0x7FFF < count) {
        printf("sceGsSetDefLoadImage: too big size\r\n");
        return 0;
    }
    pLoadImage->mWords[10] = 0ULL;
    pLoadImage->mWords[11] = 0ULL;
    pLoadImage->mWords[0] = 0ULL;
    pLoadImage->mWords[1] = 0ULL;
    pLoadImage->mWords[10] =
        (((unsigned long long)(count & 0x7FFF)) | 0x8000ULL) & 0xF3FFFFFFFFFFFFFFULL;
    pLoadImage->mWords[10] |= 0x0800000000000000ULL;
    buffer = (((unsigned long long)nTbp) << 32) | (((unsigned long long)nTbw) << 48);
    buffer |= ((unsigned long long)nPsm) << 56;
    pLoadImage->mWords[2] = buffer;
    position = (((unsigned long long)nSsx) << 32) | (((unsigned long long)nSsy) << 48);
    pLoadImage->mWords[4] = position;
    size = ((unsigned long long)nRrw) | (((unsigned long long)nRrh) << 32);
    pLoadImage->mWords[6] = size;
    pLoadImage->mWords[0] = 0x1000000000000004ULL;
    pLoadImage->mWords[1] = 0xEULL;
    pLoadImage->mWords[3] = 0x50ULL;
    pLoadImage->mWords[5] = 0x51ULL;
    pLoadImage->mWords[7] = 0x52ULL;
    pLoadImage->mWords[9] = 0x53ULL;
    pLoadImage->mWords[8] = 0ULL;
    return 6;
}

int sceGsExecLoadImage(sceGsLoadImage *pLoadImage, const void *pSource) {
    SendToGif(pLoadImage, 6U);
    SendToGif(pSource, (unsigned int)(pLoadImage->mWords[10] & 0x7FFFULL));
    return 0;
}

int sceGsSetDefStoreImage(sceGsStoreImage *pStoreImage,
                          short nSbp,
                          short nSbw,
                          short nPsm,
                          short nSsx,
                          short nSsy,
                          short nRrw,
                          short nRrh) {
    volatile unsigned int *halfWords = (volatile unsigned int *)pStoreImage;
    volatile unsigned long long *words = (volatile unsigned long long *)pStoreImage;
    unsigned long long buffer;
    unsigned long long position;
    unsigned long long size;

    words[2] = 0ULL;
    words[3] = 0ULL;
    words[2] = 0x1000000000008005ULL;
    words[3] = 0xEULL;
    buffer = ((unsigned long long)(long long)nSbp) | (((unsigned long long)nSbw) << 16);
    buffer |= ((unsigned long long)(long long)nPsm) << 24;
    words[4] = buffer;
    position = ((unsigned long long)(long long)nSsx) | (((unsigned long long)nSsy) << 16);
    words[6] = position;
    size = ((unsigned long long)(long long)nRrw) | (((unsigned long long)(long long)nRrh) << 32);
    words[8] = size;
    halfWords[1] = 0x06008000U;
    halfWords[2] = 0x13000000U;
    halfWords[3] = 0x50000006U;
    words[5] = 0x50ULL;
    words[7] = 0x51ULL;
    words[9] = 0x52ULL;
    words[11] = 0x61ULL;
    words[12] = 1ULL;
    words[13] = 0x53ULL;
    halfWords[0] = 0U;
    words[10] = 0ULL;
    return 7;
}

// ponytail: the null backend has no GS memory to read back, so pDest keeps its contents. A
// renderer fills it from the rectangle the descriptor names.
int sceGsExecStoreImage(sceGsStoreImage *pStoreImage, void *pDest) {
    (void)pStoreImage;
    (void)pDest;
    return 0;
}

// Every path is idle, as every transfer has finished.
int sceGsSyncPath(int nMode, unsigned short nTimeout) {
    (void)nMode;
    (void)nTimeout;
    return 0;
}

void sceGsSetHalfOffset(void *pDrawEnv, int nOffsetX, int nOffsetY, int nField) {
    volatile unsigned long long *words = (volatile unsigned long long *)pDrawEnv;
    unsigned long long scissor = words[6];
    long long shownX = (long long)((scissor >> 16) & 0x7FFULL);
    long long shownY = (long long)((scissor >> 48) & 0x7FFULL);
    long long deltaX = (long long)(short)nOffsetX - ((shownX + 1) >> 1);
    long long deltaY = (long long)(short)nOffsetY - ((shownY + 1) >> 1);
    unsigned long long across = (unsigned long long)deltaX << 4;
    unsigned long long down = (unsigned long long)deltaY << 4;

    if ((short)nField == 0) {
        words[4] = across | (down << 32);
    } else {
        words[4] = across | ((down + 8ULL) << 32);
    }
}

void sceGsSwapDBuff(sceGsDBuff *pDBuff, int nField) {
    if ((nField & 1) == 0) {
        sceGsPutDispEnv(&pDBuff->disp[0]);
        sceGsPutDrawEnv(&pDBuff->giftag0);
    } else {
        sceGsPutDispEnv(&pDBuff->disp[1]);
        sceGsPutDrawEnv(&pDBuff->giftag1);
    }
}

int (*sceGsSyncVCallback(int (*pfnHandler)(int)))(int) {
    GsState *state = sceGsGetGParam();
    int (*oldHandler)(int) = state->vblankHandler;

    if (pfnHandler == NULL) {
        DisableIntc(INTC_VBLANK_S);
        RemoveIntcHandler(INTC_VBLANK_S, state->handlerId);
        state->vblankHandler = NULL;
        state->handlerId = 0;
    } else {
        if (oldHandler != NULL) {
            DisableIntc(INTC_VBLANK_S);
            RemoveIntcHandler(INTC_VBLANK_S, state->handlerId);
        }
        state->vblankHandler = pfnHandler;
        state->handlerId = AddIntcHandler(INTC_VBLANK_S, pfnHandler, -1);
        EnableIntc(INTC_VBLANK_S);
        (void)HostVblankCount();
    }
    return oldHandler;
}
