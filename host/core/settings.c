// Player settings. Every host option is a FREQ_* environment variable. A settings file of
// KEY=VALUE lines ('#' starts a comment) supplies the ones the environment leaves unset: the file
// named by FREQ_CONFIG, or frequency.ini in the working directory. host/frequency.ini.example lists
// them. Runs before the other constructors that read options (priority 101), and reads with
// open()/read() because host/io/stdio.cpp's fopen() serves the game's file layer.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor(101))) static void LoadSettings(void) {
    const char *pszPath = getenv("FREQ_CONFIG");
    const int nFile = open(pszPath != NULL ? pszPath : "frequency.ini", O_RDONLY);
    if (nFile < 0) {
        return;
    }
    static char s_szText[65536];
    const ssize_t nSize = read(nFile, s_szText, sizeof(s_szText) - 1);
    close(nFile);
    s_szText[nSize > 0 ? nSize : 0] = '\0';
    for (char *pszLine = strtok(s_szText, "\r\n"); pszLine != NULL; pszLine = strtok(NULL, "\r\n")) {
        char *pszKey = pszLine + strspn(pszLine, " \t");
        char *pszValue = strchr(pszKey, '=');
        if (*pszKey == '#' || pszValue == NULL) {
            continue;
        }
        *pszValue++ = '\0';
        pszKey[strcspn(pszKey, " \t")] = '\0';
        pszValue += strspn(pszValue, " \t");
        for (size_t n = strlen(pszValue); n > 0 && strchr(" \t", pszValue[n - 1]); --n) {
            pszValue[n - 1] = '\0';
        }
        setenv(pszKey, pszValue, 0); // the environment wins over the file
    }
}

// Set one option for this run and keep it in the settings file: the file's line for the key is
// replaced, or one is added. Returns 0 when the file could not be written.
int HostSaveSetting(const char *pszKey, const char *pszValue) {
    setenv(pszKey, pszValue, 1);
    const char *pszPath = getenv("FREQ_CONFIG") != NULL ? getenv("FREQ_CONFIG") : "frequency.ini";
    static char s_szOld[65536];
    static char s_szNew[65536 + 256];
    ssize_t nSize = 0;
    const int nIn = open(pszPath, O_RDONLY);
    if (nIn >= 0) {
        nSize = read(nIn, s_szOld, sizeof(s_szOld) - 1);
        close(nIn);
    }
    s_szOld[nSize > 0 ? nSize : 0] = '\0';
    const size_t nKey = strlen(pszKey);
    size_t nUsed = 0;
    for (char *pszLine = s_szOld; *pszLine != '\0';) {
        char *pszEnd = pszLine + strcspn(pszLine, "\n");
        const size_t nLine = (size_t)(pszEnd - pszLine) + (*pszEnd == '\n');
        const char *pszText = pszLine + strspn(pszLine, " \t");
        if (!(strncmp(pszText, pszKey, nKey) == 0 && strchr(" \t=", pszText[nKey]) != NULL)) {
            memcpy(s_szNew + nUsed, pszLine, nLine);
            nUsed += nLine;
        }
        pszLine += nLine;
    }
    if (nUsed > 0 && s_szNew[nUsed - 1] != '\n') {
        s_szNew[nUsed++] = '\n';
    }
    nUsed += (size_t)snprintf(s_szNew + nUsed, 256, "%s=%s\n", pszKey, pszValue);
    const int nOut = open(pszPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (nOut < 0) {
        return 0;
    }
    const int bOk = write(nOut, s_szNew, nUsed) == (ssize_t)nUsed;
    close(nOut);
    return bOk;
}
