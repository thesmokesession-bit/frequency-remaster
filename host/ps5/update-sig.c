// The updater's check of a release (ps5.c): an Ed25519 signature over the SHA-256 digest of its
// eboot.bin, made with the private key the person who publishes releases keeps off GitHub, so a
// release whose signature this public key does not accept is never installed. Signing:
//   openssl dgst -sha256 -binary eboot.bin > eboot.bin.sha256
//   openssl pkeyutl -sign -inkey frequency-update.key -rawin -in eboot.bin.sha256 -out eboot.bin.sig
// Built with -DUPDATE_SIG_TEST it is a program: update-sig <file> <signature> prints VALID or
// INVALID.
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>

static const unsigned char kUpdateKey[32] = {
    0x21, 0xaa, 0x6e, 0xd6, 0x25, 0xab, 0x60, 0xc6, 0xed, 0xde, 0x89, 0xcd, 0x3d, 0x9c, 0x3a, 0x4c,
    0xb0, 0x3f, 0xcc, 0x4d, 0xd9, 0xa8, 0x81, 0x9f, 0xc4, 0x94, 0x3b, 0x76, 0x9b, 0xe8, 0x4f, 0x57};

// 1 when pSignature (nSignature bytes) is the release key's signature of the file at pszPath.
int HostUpdateSignatureValid(const char *pszPath, const unsigned char *pSignature, size_t nSignature) {
    // open() and pread(), as host/core/logs.cpp reads a file: the game's fopen() and read() go
    // through its own file layer (host/io/stdio.cpp, src/os/filelog.cpp).
    const int nFile = nSignature == 64 ? open(pszPath, O_RDONLY | O_CLOEXEC) : -1;
    if (nFile < 0) {
        return 0;
    }
    unsigned char abDigest[EVP_MAX_MD_SIZE];
    unsigned int nDigest = 0;
    static unsigned char s_abChunk[1 << 16]; // only the updater's thread checks a release
    EVP_MD_CTX *pHash = EVP_MD_CTX_new();
    int bOk = pHash != NULL && EVP_DigestInit_ex(pHash, EVP_sha256(), NULL) == 1;
    long nHashed = 0;
    ssize_t nRead = 0;
    while (bOk && (nRead = pread(nFile, s_abChunk, sizeof(s_abChunk), nHashed)) > 0) {
        bOk = EVP_DigestUpdate(pHash, s_abChunk, (size_t)nRead) == 1;
        nHashed += nRead;
    }
    bOk = bOk && nRead == 0 && EVP_DigestFinal_ex(pHash, abDigest, &nDigest) == 1;
    EVP_MD_CTX_free(pHash);
    close(nFile);
    char szDigest[2 * 32 + 1] = "none";
    for (unsigned int i = 0; bOk && i < nDigest && i < 32; ++i) {
        snprintf(szDigest + 2 * i, 3, "%02x", abDigest[i]);
    }
    EVP_PKEY *pKey = bOk ? EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, kUpdateKey, 32) : NULL;
    EVP_MD_CTX *pVerify = pKey != NULL ? EVP_MD_CTX_new() : NULL;
    const int bInit = pVerify != NULL && EVP_DigestVerifyInit(pVerify, NULL, NULL, NULL, pKey) == 1;
    const int bValid = bInit && EVP_DigestVerify(pVerify, pSignature, nSignature, abDigest, nDigest) == 1;
    // The digest, the bytes hashed, and the signature's ends in the log: a refused release can be
    // compared with the published file's.
    fprintf(stderr,
            "PS5: update: sha256 %s of %ld bytes, signature %02x%02x...%02x%02x, key %s, check %s\n",
            szDigest, nHashed, pSignature[0], pSignature[1], pSignature[62], pSignature[63],
            pKey != NULL ? "loaded" : "not loaded", bInit ? (bValid ? "passed" : "failed") : "not run");
    if (!bValid) {
        ERR_print_errors_fp(stderr);
    }
    EVP_MD_CTX_free(pVerify);
    EVP_PKEY_free(pKey);
    return bValid;
}

#ifdef UPDATE_SIG_TEST
int main(int argc, char **argv) {
    unsigned char abSignature[128];
    FILE *pFile = argc == 3 ? fopen(argv[2], "rb") : NULL;
    const size_t nSignature = pFile != NULL ? fread(abSignature, 1, sizeof(abSignature), pFile) : 0;
    if (pFile != NULL) {
        fclose(pFile);
    }
    const int bValid = HostUpdateSignatureValid(argv[1], abSignature, nSignature);
    puts(bValid ? "VALID" : "INVALID");
    return bValid ? 0 : 1;
}
#endif
