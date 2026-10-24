// The movie decoder: the part of Sony's libmpeg the ezmpeg sample in sce/ee/sample calls, and the
// sample's IPU input ring, over libavcodec (FFmpeg, LGPL, linked dynamically) in place of the IPU.
//
// The sample runs unchanged around it. cutscene.c reads the stream and demultiplexes it here, the
// video callback fills the input ring (vibuf.c), the decode thread takes each picture from
// sceMpegGetPicture() as the IPU's colour converter writes it (RGB32 16x16 macroblocks, column by
// column), and the display (disp.c) uploads and shows it from its blank handler.
//
// ponytail: one movie at a time (one decoder, one input ring), which is all the game plays.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <eekernel.h>
#include <ezmpeg.h>
#include <ezmpeg/vibuf.h>
#include <libmpeg.h>
#include <libsdr.h>

#include "../gfx-null/gfxnull.h"
#include "ezmpeg/vobuf.h"

extern "C" void HostUseSystemHeap(int bOn); // host/core/heap.c
void HostBreadcrumb(const char *pszName);   // host/io/pad.cpp
extern "C" VoBuf voBuf;                     // src/app/cutscene.c

static_assert(sizeof(ViBuf) <= sizeof(VideoDec::vibuf), "the input ring overruns VideoDec");

namespace {

constexpr int kSectorShift = 11;
constexpr int kSectorSize = 1 << kSectorShift;
constexpr int kMacroblock = 16;
constexpr unsigned char kCscAlpha = 0x80;
// disp.c's state of a queued picture that has had its field 0 blank.
constexpr int kTagFirstFieldShown = 1;
// Time stamps held between the input ring and the parser, and the stamp of one picture at 29.97 Hz.
constexpr int kStampCount = 256;
constexpr long long kPtsPerPicture = 3003;
// Blanks late past which the decoder skips the pictures nothing refers to (B pictures).
constexpr long kSkipLate = 6;

// sceMpegSetCallbackSlot() slots: no data (yield to the reader), and how many there are.
constexpr int kSlotNoData = 1;
constexpr int kSlotCount = 8;

// Stream keys as libmpeg builds them: the stream id above 32 bits, and for private stream 1 the
// payload's first word below. Each type's key and the bits a packet must match.
constexpr unsigned long long kStreamTemplates[10][2] = {
    {0xe000000000ULL, 0xff00000000ULL},
    {0xbdffc00000ULL, 0xffffffffffULL},
    {0xbdffa00000ULL, 0xffffffffffULL},
    {0xbdffa10000ULL, 0xffffffffffULL},
    {0xbdff900000ULL, 0xffffffffffULL},
    {0xc000000000ULL, 0xff00000000ULL},
    {0xbd80000000ULL, 0xffff000000ULL},
    {0xbda0000000ULL, 0xffff000000ULL},
    {0xbd88000000ULL, 0xffff000000ULL},
    {0xbd90000000ULL, 0xffff000000ULL},
};
constexpr int kMaxStreams = 8;

struct Stream {
    unsigned long long llKey;
    unsigned long long llMask;
    sceMpegCallback pfnCallback;
    void *pData;
};

struct Decoder {
    Stream aStreams[kMaxStreams];
    int nStreams;
    struct {
        void *pfnCallback;
        void *pData;
    } aSlots[kSlotCount];
    AVCodecContext *pCodec;
    AVCodecParserContext *pParser;
    AVPacket *pPacket;
    AVFrame *pFrame;
    bool bPacketPending;
    bool bParserFlushed;
    bool bDraining;
    bool bEnd;
    bool bReset;
    int nPictures;
    // The blank of the INTRO_MOVIE_STARTED breadcrumb, or 0 before the display starts.
    unsigned nStartVblank;
    unsigned char *pLastPicture;
    unsigned char abInput[0x10000];
    int nInput;
    int nInputUsed;
    // Stream position of abInput[0], and of the next byte TakeInput() takes.
    long long llInputPos;
    long long llTakenPos;
    // The PES time stamps not yet handed to the parser: where each packet's data starts in the
    // stream, and its PTS. A ring, oldest at nStampHead.
    struct {
        long long llPos;
        long long llPts;
    } aStamps[kStampCount];
    int nStampHead;
    int nStamps;
    // The PTS of picture 0 and of the last picture, in 90 kHz units.
    long long llFirstPts;
    long long llLastPts;
    // Pictures dropped late and turns spent holding early ones, reported at the end.
    int nDropped;
    int nHeld;
};

Decoder g_decoder;
ViBuf *g_pInput;
bool g_bInputFlushed;

struct SystemHeap {
    SystemHeap() {
        HostUseSystemHeap(1);
    }
    ~SystemHeap() {
        HostUseSystemHeap(0);
    }
};

// Take what the input ring holds, whole sectors until the stream is flushed, as the IPU's input
// DMA does. Returns the bytes copied to pOut.
int TakeInput(unsigned char *pOut, int nMax) {
    ViBuf *pInput = g_pInput;
    WaitSema(pInput->mSemaId);
    int nTake = pInput->mBufferedBytes;
    if (nTake > nMax) {
        nTake = nMax;
    }
    if (!g_bInputFlushed) {
        nTake &= ~(kSectorSize - 1);
    }
    const int nOffset = (pInput->mReadSectors << kSectorShift) % pInput->mSize;
    const int nFirst = nTake < pInput->mSize - nOffset ? nTake : pInput->mSize - nOffset;
    memcpy(pOut, pInput->mData + nOffset, nFirst);
    memcpy(pOut + nFirst, pInput->mData, nTake - nFirst);
    pInput->mReadSectors += nTake >> kSectorShift;
    pInput->mBufferedBytes -= nTake; // A flushed tail leaves a part sector unread; nothing follows.
    // The stamps of the packets whose data starts in what was taken move to the parser's side.
    Decoder &decoder = g_decoder;
    while (pInput->mTimeStampCount > 0) {
        const int nOldest = (pInput->mTimeStampIndex - pInput->mTimeStampCount +
                             pInput->mTimeStampCapacity) % pInput->mTimeStampCapacity;
        const ViTimeStamp &stamp = pInput->mTimeStamps[nOldest];
        const int nDelta = (stamp.mOffset - nOffset + pInput->mSize) % pInput->mSize;
        if (nDelta >= nTake) {
            break;
        }
        if (decoder.nStamps < kStampCount && stamp.mFirst >= 0) {
            auto &entry = decoder.aStamps[(decoder.nStampHead + decoder.nStamps++) % kStampCount];
            entry.llPos = decoder.llTakenPos + nDelta;
            entry.llPts = stamp.mFirst;
        }
        --pInput->mTimeStampCount;
    }
    decoder.llTakenPos += nTake;
    SignalSema(pInput->mSemaId);
    return nTake;
}

// IPU colour conversion (ITU-R BT.601, video range, each chroma sample shared by 2x2 pixels) into
// 16x16 RGB32 macroblocks, column by column, the order disp.c uploads them in.
void ConvertPicture(const AVFrame *pFrame, unsigned char *pOut) {
    const int nColumns = pFrame->width / kMacroblock;
    const int nRows = pFrame->height / kMacroblock;
    for (int nColumn = 0; nColumn < nColumns; ++nColumn) {
        for (int nRow = 0; nRow < nRows; ++nRow) {
            for (int y = nRow * kMacroblock; y < (nRow + 1) * kMacroblock; ++y) {
                const unsigned char *pY = pFrame->data[0] + y * pFrame->linesize[0];
                const unsigned char *pU = pFrame->data[1] + (y >> 1) * pFrame->linesize[1];
                const unsigned char *pV = pFrame->data[2] + (y >> 1) * pFrame->linesize[2];
                for (int x = nColumn * kMacroblock; x < (nColumn + 1) * kMacroblock; ++x) {
                    const int c = 298 * (pY[x] - 16);
                    const int d = pU[x >> 1] - 128;
                    const int e = pV[x >> 1] - 128;
                    const int aRgb[3] = {(c + 409 * e + 128) >> 8,
                                         (c - 100 * d - 208 * e + 128) >> 8,
                                         (c + 516 * d + 128) >> 8};
                    for (int v : aRgb) {
                        *pOut++ = static_cast<unsigned char>(v < 0 ? 0 : v > 255 ? 255 : v);
                    }
                    *pOut++ = kCscAlpha;
                }
            }
        }
    }
}

// The movie clock, as the console player keeps it: picture n (its PTS less the first picture's,
// over one picture's 3003 ticks) is due at blank start + 2 + 2n, and disp.c puts a picture up at
// a field 0 blank and holds it for the field 1 blank. A picture made now goes up after every
// queued picture not yet shown, so its blank follows from the queue and the field. Returns how
// many blanks after its due blank picture n would go up, negative when early, 0 before the display
// starts.
long Lateness(int nPicture) {
    const unsigned nStart = g_decoder.nStartVblank;
    if (nStart == 0) {
        return 0; // Still filling the queue before the display starts.
    }
    const long nNow = HostVblankCount();
    const long nNextField0 = nNow + (((*HOST_GS_CSR & HOST_GS_CSR_FIELD) != 0) ? 1 : 2);
    const int nQueued = voBuf.count;
    const int *pHead = static_cast<const int *>(voBufGetTag(&voBuf));
    long nShown;
    if (pHead != nullptr && *pHead == kTagFirstFieldShown) {
        nShown = nNow + 2 * nQueued; // The head is up now and leaves after this field 1.
    } else {
        nShown = nNextField0 + 2L * nQueued;
    }
    return nShown - (static_cast<long>(nStart) + 2 + 2L * nPicture);
}

// The picture number of a decoded frame, from its PTS.
int PictureNumber(const AVFrame *pFrame) {
    Decoder &decoder = g_decoder;
    long long llPts = pFrame->pts != AV_NOPTS_VALUE ? pFrame->pts : pFrame->best_effort_timestamp;
    if (llPts == AV_NOPTS_VALUE) {
        llPts = decoder.nPictures == 0 ? 0 : decoder.llLastPts + kPtsPerPicture;
    }
    if (decoder.nPictures++ == 0) {
        decoder.llFirstPts = llPts;
    }
    decoder.llLastPts = llPts;
    return static_cast<int>((llPts - decoder.llFirstPts + kPtsPerPicture / 2) / kPtsPerPicture);
}

int InvokeSlot(sceMpeg *pMpeg, int nSlot) {
    auto pfnCallback = reinterpret_cast<sceMpegCallback>(g_decoder.aSlots[nSlot].pfnCallback);
    return pfnCallback != nullptr ? pfnCallback(pMpeg, nullptr, g_decoder.aSlots[nSlot].pData) : 0;
}

// Byte i of the ring the stream sits in.
struct RingReader {
    unsigned char *pStart;
    unsigned char *pBuffer;
    int nBufferSize;
    unsigned char *At(int i) const {
        unsigned char *p = pStart + i;
        return (pBuffer != nullptr && p >= pBuffer + nBufferSize) ? p - nBufferSize : p;
    }
    unsigned Byte(int i) const {
        return *At(i);
    }
    unsigned Word(int i) const {
        return (Byte(i) << 24) | (Byte(i + 1) << 16) | (Byte(i + 2) << 8) | Byte(i + 3);
    }
    long long Stamp(int i) const {
        return (static_cast<long long>((Byte(i) >> 1) & 7) << 30) | (Byte(i + 1) << 22) |
               ((Byte(i + 2) >> 1) << 15) | (Byte(i + 3) << 7) | (Byte(i + 4) >> 1);
    }
};

} // namespace

extern "C" {

int sceMpegInit(void) {
    return 0;
}

void *sceMpegCreateDecoderContext(void *pDecoder, void *pWork, int) {
    sceMpeg *pMpeg = static_cast<sceMpeg *>(pDecoder);
    Decoder &decoder = g_decoder;
    memset(pMpeg, 0, sizeof(*pMpeg));
    pMpeg->pts = pMpeg->dts = pMpeg->pts2nd = pMpeg->dts2nd = -1;
    pMpeg->pContext = &decoder;

    const SystemHeap systemHeap;
    memset(&decoder, 0, sizeof(decoder));
    const AVCodec *pCodec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    decoder.pCodec = avcodec_alloc_context3(pCodec);
    decoder.pCodec->thread_count = 1;
    decoder.pParser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    decoder.pPacket = av_packet_alloc();
    decoder.pFrame = av_frame_alloc();
    if (avcodec_open2(decoder.pCodec, pCodec, nullptr) < 0 || decoder.pParser == nullptr) {
        fprintf(stderr, "host: cannot open libavcodec's MPEG-2 decoder\n");
    }
    return pWork;
}

int sceMpegDelete(void *) {
    Decoder &decoder = g_decoder;
    fprintf(stderr,
            "host: movie decoded %d pictures, dropped %d late, held early ones %d turns\n",
            decoder.nPictures,
            decoder.nDropped,
            decoder.nHeld);
    const SystemHeap systemHeap;
    av_parser_close(decoder.pParser);
    avcodec_free_context(&decoder.pCodec);
    av_packet_free(&decoder.pPacket);
    av_frame_free(&decoder.pFrame);
    return 1;
}

void *sceMpegSetCallbackSlot(void *, int nSlot, void *pfnCallback, void *pData) {
    void *pfnOld = g_decoder.aSlots[nSlot].pfnCallback;
    g_decoder.aSlots[nSlot].pfnCallback = pfnCallback;
    g_decoder.aSlots[nSlot].pData = pData;
    return pfnOld;
}

sceMpegCallback
sceMpegAddStrCallback(void *, int nType, int nChannel, sceMpegCallback pfnCallback, void *pData) {
    Decoder &decoder = g_decoder;
    const unsigned long long llMask = kStreamTemplates[nType][1];
    const int nShift = llMask == 0xff00000000ULL ? 32 : llMask == 0xffff000000ULL ? 24 : 0;
    const unsigned long long llKey =
        kStreamTemplates[nType][0] | (static_cast<unsigned long long>(nChannel) << nShift);
    for (int i = 0; i < decoder.nStreams; ++i) {
        if (decoder.aStreams[i].llKey == llKey) {
            const sceMpegCallback pfnOld = decoder.aStreams[i].pfnCallback;
            decoder.aStreams[i] = {llKey, llMask, pfnCallback, pData};
            return pfnOld;
        }
    }
    if (decoder.nStreams < kMaxStreams) {
        decoder.aStreams[decoder.nStreams++] = {llKey, llMask, pfnCallback, pData};
    }
    return nullptr;
}

void sceMpegSetDecodeMode(void *, int, int, int) {
}

// Hand each whole PES packet of the program stream to its stream's callback. Stops at a packet
// that is not all there, or that its callback refuses, and returns the bytes handed over.
int sceMpegDemuxPssRing(
    sceMpeg *pMpeg, unsigned char *pStart, int nSize, unsigned char *pBuffer, int nBufferSize) {
    Decoder &decoder = g_decoder;
    const RingReader ring = {pStart, pBuffer, nBufferSize};
    int nPosition = 0;
    int nConsumed = 0;
    while (nPosition + 6 <= nSize) {
        const unsigned nCode = ring.Word(nPosition);
        if (nCode == 0x1ba) { // Pack header (MPEG-2), and its stuffing.
            if (nPosition + 14 > nSize) {
                break;
            }
            nPosition += 14 + (ring.Byte(nPosition + 13) & 7);
            continue;
        }
        if ((nCode >> 8) != 1 || nCode == 0x1b9) {
            break;
        }
        const int nLength = 6 + ((ring.Byte(nPosition + 4) << 8) | ring.Byte(nPosition + 5));
        if (nPosition + nLength > nSize) {
            break;
        }
        const unsigned nStreamId = nCode & 0xff;
        if (nStreamId == 0xe0 || nStreamId == 0xbd ||
            (nStreamId >= 0xc0 && nStreamId < 0xf0 && nStreamId != 0xbe)) {
            const unsigned nFlags = ring.Byte(nPosition + 7);
            const int nData = nPosition + 9 + ring.Byte(nPosition + 8);
            sceMpegCbDataStr data = {};
            data.type = sceMpegCbStr;
            data.header = ring.At(nPosition);
            data.data = ring.At(nData);
            data.len = static_cast<unsigned>(nPosition + nLength - nData);
            data.pts = (nFlags & 0x80) != 0 ? ring.Stamp(nPosition + 9) : -1;
            data.dts = (nFlags & 0xc0) == 0xc0 ? ring.Stamp(nPosition + 14) : -1;
            unsigned long long llKey = static_cast<unsigned long long>(nStreamId) << 32;
            if (nStreamId == 0xbd) {
                llKey |= ring.Word(nData);
            }
            for (int i = 0; i < decoder.nStreams; ++i) {
                const Stream &stream = decoder.aStreams[i];
                if ((llKey & stream.llMask) == stream.llKey) {
                    if (stream.pfnCallback(pMpeg, &data, stream.pData) == 0) {
                        return nConsumed;
                    }
                    break;
                }
            }
        }
        nPosition += nLength;
        nConsumed = nPosition;
    }
    // ponytail: a stopped decoder drops what it has not read, so after a skip the reader can
    // still flush into the ring instead of waiting on a reader that is gone.
    if (decoder.bReset && g_pInput != nullptr) {
        WaitSema(g_pInput->mSemaId);
        g_pInput->mReadSectors += g_pInput->mBufferedBytes >> kSectorShift;
        g_pInput->mBufferedBytes &= kSectorSize - 1;
        SignalSema(g_pInput->mSemaId);
    }
    return nConsumed;
}

// Decode the next picture into pPicture. At the end of the stream the last picture is shown
// again, and sceMpegIsEnd() turns true.
int sceMpegGetPicture(void *pDecoder, void *pPicture, int) {
    sceMpeg *pMpeg = static_cast<sceMpeg *>(pDecoder);
    Decoder &decoder = g_decoder;
    unsigned char *pOut = static_cast<unsigned char *>(pPicture);
    int nPicture = 0;
    for (;;) {
        int nResult;
        {
            const SystemHeap systemHeap;
            nResult = avcodec_receive_frame(decoder.pCodec, decoder.pFrame);
            if (nResult == 0) {
                nPicture = PictureNumber(decoder.pFrame);
                // A picture more than a blank late is dropped, and the display holds the last
                // one. Far behind, the pictures nothing refers to are not even decoded.
                const long nLate = Lateness(nPicture);
                decoder.pCodec->skip_frame = nLate > kSkipLate ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
                if (nLate > 1) {
                    ++decoder.nDropped;
                    av_frame_unref(decoder.pFrame);
                    continue;
                }
            }
            if (nResult == 0) {
                ConvertPicture(decoder.pFrame, pOut);
                pMpeg->width = decoder.pFrame->width;
                pMpeg->height = decoder.pFrame->height;
                av_frame_unref(decoder.pFrame);
            } else if (nResult == AVERROR(EAGAIN) && decoder.bPacketPending) {
                if (avcodec_send_packet(decoder.pCodec, decoder.pPacket) != AVERROR(EAGAIN)) {
                    decoder.bPacketPending = false;
                }
                continue;
            } else if (nResult == AVERROR(EAGAIN) && decoder.nInputUsed < decoder.nInput) {
                // Each run of bytes up to the next stamped packet goes in with its packet's PTS.
                const long long llPos = decoder.llInputPos + decoder.nInputUsed;
                long long llPts = AV_NOPTS_VALUE;
                while (decoder.nStamps > 0 && decoder.aStamps[decoder.nStampHead].llPos <= llPos) {
                    llPts = decoder.aStamps[decoder.nStampHead].llPts;
                    decoder.nStampHead = (decoder.nStampHead + 1) % kStampCount;
                    --decoder.nStamps;
                }
                int nRun = decoder.nInput - decoder.nInputUsed;
                if (decoder.nStamps > 0) {
                    nRun = std::min<long long>(nRun, decoder.aStamps[decoder.nStampHead].llPos - llPos);
                }
                decoder.nInputUsed += av_parser_parse2(decoder.pParser,
                                                       decoder.pCodec,
                                                       &decoder.pPacket->data,
                                                       &decoder.pPacket->size,
                                                       decoder.abInput + decoder.nInputUsed,
                                                       nRun,
                                                       llPts,
                                                       AV_NOPTS_VALUE,
                                                       llPos);
                decoder.pPacket->pts = decoder.pParser->pts;
                decoder.pPacket->dts = decoder.pParser->dts;
                decoder.bPacketPending = decoder.pPacket->size > 0;
                continue;
            }
        }
        if (nResult == 0) {
            // An early picture waits for its blank, and the display holds the last one meanwhile.
            while (Lateness(nPicture) < -1) {
                ++decoder.nHeld;
                InvokeSlot(pMpeg, kSlotNoData);
            }
            decoder.pLastPicture = pOut;
            pMpeg->frameCount = nPicture;
            return 1;
        }
        if (nResult != AVERROR(EAGAIN)) { // The end, or an error the decoder cannot pass.
            if (decoder.pLastPicture != nullptr && decoder.pLastPicture != pOut) {
                memcpy(pOut, decoder.pLastPicture, pMpeg->width * pMpeg->height * 4);
            }
            decoder.bEnd = true;
            return 0;
        }
        decoder.llInputPos = decoder.llTakenPos;
        decoder.nInput = TakeInput(decoder.abInput, sizeof(decoder.abInput));
        decoder.nInputUsed = 0;
        if (decoder.nInput > 0) {
            continue;
        }
        if (g_bInputFlushed) {
            const SystemHeap systemHeap;
            if (!decoder.bParserFlushed) {
                decoder.bParserFlushed = true;
                av_parser_parse2(decoder.pParser,
                                 decoder.pCodec,
                                 &decoder.pPacket->data,
                                 &decoder.pPacket->size,
                                 nullptr,
                                 0,
                                 AV_NOPTS_VALUE,
                                 AV_NOPTS_VALUE,
                                 0);
                decoder.bPacketPending = decoder.pPacket->size > 0;
            } else if (!decoder.bDraining) {
                decoder.bDraining = true;
                avcodec_send_packet(decoder.pCodec, nullptr);
            }
            continue;
        }
        InvokeSlot(pMpeg, kSlotNoData); // Yields to the reader.
    }
}

int sceMpegIsEnd(void *) {
    return g_decoder.bEnd;
}

int sceMpegIsRefBuffEmpty(void *) {
    return g_decoder.bEnd || g_decoder.nPictures == 0;
}

void sceMpegReset(void *) {
    g_decoder.bReset = true;
}

// The input ring (sceDmaCreateQueueSemaphore() is the name the image gives viBufCreate()). Its
// sectors are read by TakeInput() rather than sent to the IPU, so the DMA calls do nothing.
void sceDmaCreateQueueSemaphore(
    ViBuf *pInput, void *pData, void *pTag, int nTags, void *pTimeStamps, int nTimeStamps) {
    memset(pInput, 0, sizeof(*pInput));
    pInput->mData = static_cast<unsigned char *>(pData);
    pInput->mTagBase = static_cast<int>(reinterpret_cast<uintptr_t>(pTag));
    pInput->mCapacitySectors = nTags;
    pInput->mSize = nTags << kSectorShift;
    pInput->mTimeStamps = static_cast<ViTimeStamp *>(pTimeStamps);
    pInput->mTimeStampCapacity = nTimeStamps;
    SemaParam semaphore = {};
    semaphore.initCount = 1;
    semaphore.maxCount = 1;
    pInput->mSemaId = CreateSema(&semaphore);
    g_pInput = pInput;
    g_bInputFlushed = false;
}

int sceDmaDeleteQueueSemaphore(ViBuf *pInput) {
    DeleteSema(pInput->mSemaId);
    g_pInput = nullptr;
    return 1;
}

int viBufReset(ViBuf *pInput) {
    WaitSema(pInput->mSemaId);
    pInput->mReadSectors = pInput->mBufferedSectors = pInput->mBufferedBytes = 0;
    pInput->mTimeStampCount = pInput->mTimeStampIndex = 0;
    SignalSema(pInput->mSemaId);
    g_bInputFlushed = false;
    return 1;
}

int viBufAddDMA(ViBuf *) {
    return 1;
}

int viBufStopDMA(ViBuf *) {
    return 1;
}

int viBufRestartDMA(ViBuf *) {
    return 1;
}

int viBufGetTs(ViBuf *, long long *pStamps) {
    pStamps[0] = pStamps[1] = -1;
    return 0;
}

void viBufModifyPts(ViBuf *, ViTimeStamp *) {
}

void viBufFlush(ViBuf *) {
    g_bInputFlushed = true;
}

// cutscene.c's switchThread(). The reader, the decoder, and the sample's idle thread rotate the
// ready queue while they wait on a blank, which on the console costs nothing but here took a whole
// host core from the renderer. Each turn rests 0.5 ms, holding the processor as a wait would.
void HostMovieIdle(void) {
    usleep(500);
}

// The first frame of the intro movie goes up at the second blank after this.
void HostMovieStarted(void) {
    g_decoder.nStartVblank = HostVblankCount();
    HostBreadcrumb("INTRO_MOVIE_STARTED");
}

// The movie's sound driver calls (audiodec.c is compiled with sceSdRemote renamed to this).
// Movie audio is silent: the PCM the sample sends with sceSifSetDma() is dropped by
// host/kernel/iop.cpp. The SPU2 block transfer the sample starts is modelled only as a clock, so
// that the sample's ring drains at 48 kHz stereo 16-bit (192000 bytes a second) as on the console:
//   0x80e0 sceSdBlockTrans(core, mode, IOP buffer, size, start)  starts or (mode 2) stops it,
//   0x8100 sceSdBlockTransStatus(core) returns the IOP address the SPU2 reads now.
// To hear it, keep the SIF DMA bytes per IOP address and play the ring from that read position.
int HostMovieSdRemote(int nBlocking, int nCommand, ...) {
    static timespec start;
    static unsigned nBuffer, nSize, nOffset;
    va_list arguments;
    va_start(arguments, nCommand);
    int aArguments[5];
    for (int &nArgument : aArguments) {
        nArgument = va_arg(arguments, int);
    }
    va_end(arguments);
    if (nCommand == 0x80e0 && aArguments[1] != 2) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        nBuffer = aArguments[2];
        nSize = aArguments[3];
        nOffset = aArguments[4] - nBuffer;
        return 0;
    }
    if (nCommand == 0x80e0) {
        nSize = 0;
        return 0;
    }
    if (nCommand == 0x8100) {
        if (nSize == 0) {
            return 0;
        }
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long long llBytes = ((now.tv_sec - start.tv_sec) * 1000000000LL +
                                   (now.tv_nsec - start.tv_nsec)) * 192000 / 1000000000LL;
        return static_cast<int>(nBuffer + (nOffset + llBytes) % nSize);
    }
    return sceSdRemote(nBlocking,
                       nCommand,
                       aArguments[0],
                       aArguments[1],
                       aArguments[2],
                       aArguments[3],
                       aArguments[4]);
}

} // extern "C"
