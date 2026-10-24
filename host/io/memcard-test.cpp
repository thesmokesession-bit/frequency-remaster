// A check of the memory card over a scratch directory, outside the game. From the root:
//
//     g++ -std=gnu++17 -Isce/ee/include host/io/memcard.cpp host/io/memcard-test.cpp -o mc-test
//     FREQ_MC=$(mktemp -d)/card ./mc-test

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <libmc.h>

namespace {

int Result() {
    int nCommand;
    int nResult;
    assert(sceMcSync(sceMcWait, &nCommand, &nResult) == sceMcExecFinish);
    return nResult;
}

} // namespace

int main() {
    int nType;
    int nFree;
    int nFormat;
    sceMcInitLibrary();
    sceMcGetInfo(0, 0, &nType, &nFree, &nFormat);
    assert(Result() == sceMcResSucceed && nType == sceMcTypePS2 && nFormat == 1 && nFree == 8000);
    sceMcGetInfo(1, 0, &nType, &nFree, &nFormat);
    assert(Result() < sceMcResNoFormat && nType == sceMcTypeNoCard);

    sceMcMkdir(0, 0, "/SAVE");
    assert(Result() == sceMcResSucceed);
    sceMcMkdir(0, 0, "/SAVE");
    assert(Result() == sceMcResNoEntry);
    sceMcOpen(0, 0, "/SAVE/missing.dat", sceMcFileAttrReadable);
    assert(Result() == sceMcResNoEntry);

    sceMcOpen(0, 0, "/SAVE/data.dat", sceMcFileCreateFile | sceMcFileAttrWriteable);
    const int nFile = Result();
    assert(nFile >= 0);
    sceMcWrite(nFile, "aloud", 5);
    assert(Result() == 5);
    sceMcSeek(nFile, 1, 0);
    assert(Result() == 1);
    sceMcClose(nFile);
    assert(Result() == sceMcResSucceed);

    sceMcOpen(0, 0, "/SAVE/data.dat", sceMcFileAttrReadable);
    const int nRead = Result();
    char szText[8] = {};
    sceMcSeek(nRead, 1, 0);
    assert(Result() == 1);
    sceMcRead(nRead, szText, sizeof(szText));
    assert(Result() == 4 && strcmp(szText, "loud") == 0);
    sceMcClose(nRead);
    assert(Result() == sceMcResSucceed);

    sceMcTblGetDir aEntries[8];
    sceMcGetDir(0, 0, "/*", 0, 8, aEntries);
    assert(Result() == 1 && strcmp(reinterpret_cast<char *>(aEntries[0].EntryName), "SAVE") == 0);
    sceMcGetDir(0, 0, "/SAVE/*", 0, 8, aEntries);
    assert(Result() == 3);
    sceMcGetDir(0, 0, "/SAVE/*.dat", 0, 8, aEntries);
    assert(Result() == 1 && aEntries[0].FileSizeByte == 5);

    sceMcDelete(0, 0, "/SAVE");
    assert(Result() == sceMcResNotEmpty);
    sceMcDelete(0, 0, "/SAVE/data.dat");
    assert(Result() == sceMcResSucceed);
    sceMcDelete(0, 0, "/SAVE");
    assert(Result() == sceMcResSucceed);
    assert(sceMcSync(sceMcNoWait, nullptr, nullptr) == sceMcExecIdle);

    printf("MEMCARD_TEST_OK\n");
    return 0;
}
