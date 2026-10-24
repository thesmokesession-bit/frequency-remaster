#include "os/formatstring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

// The GetDirectoryFromPath() buffer ends where the Rnd::MakeString() buffer begins.
constexpr int kDirectoryBufferSize = 0x100;

// NTSC-U/C: 0x008de290, PAL: 0x00923250
char g_szDirectoryBuffer[kDirectoryBufferSize] = {};

// The size is not recovered: nothing else in the image references the region, and the format runs
// unbounded. The value below is a placeholder rather than a recovered size.
constexpr int kFormatStringBufferSize = 1024;

// NTSC-U/C: 0x008de390, PAL: 0x00923350
char g_szFormatStringBuffer[kFormatStringBufferSize] = {};

} // namespace

const char *Rnd::MakeString(const char *pszFormat, ...) {
    va_list args;
    va_start(args, pszFormat);
#ifdef RESONANCE_HOST
    // A caller may pass the buffer back in: TnlGemManager::AddMeshKind() formats "%s%d.mm" from the
    // "gem_ghost%d" result. The console's newlib vsprintf copies a %s argument onto itself in place
    // unharmed. glibc's vsprintf ends the destination string first, so the argument reads as empty
    // ("0.mm"). The text is built aside and copied in.
    char szFormatted[kFormatStringBufferSize];
    vsnprintf(szFormatted, sizeof(szFormatted), pszFormat, args);
    strcpy(g_szFormatStringBuffer, szFormatted);
#else
    vsprintf(g_szFormatStringBuffer, pszFormat, args);
#endif
    va_end(args);
    return g_szFormatStringBuffer;
}

const char *GetDirectoryFromPath(const char *pszPath) {
    strcpy(g_szDirectoryBuffer, pszPath);
    char *pszSeparator = strrchr(g_szDirectoryBuffer, '/');
    if (pszSeparator == nullptr) {
        pszSeparator = strrchr(g_szDirectoryBuffer, '\\');
    }
    if (pszSeparator != nullptr) {
        *pszSeparator = '\0';
    } else {
        g_szDirectoryBuffer[0] = '\0';
    }
    return g_szDirectoryBuffer;
}
