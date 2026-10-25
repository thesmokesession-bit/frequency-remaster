// Player settings. Every host option is a FREQ_* environment variable. A settings file of
// KEY=VALUE lines ('#' starts a comment) supplies the ones the environment leaves unset: the file
// named by FREQ_CONFIG, or frequency.ini in the working directory. host/frequency.ini.example lists
// them. Runs before the other constructors that read options (priority 101), and reads with
// open()/read() because host/io/stdio.cpp's fopen() serves the game's file layer.

#include <fcntl.h>
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
