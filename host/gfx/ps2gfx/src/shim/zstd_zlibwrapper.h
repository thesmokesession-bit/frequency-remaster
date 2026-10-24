// Stand-in for Play!'s zstd zlib wrapper: Framework's zip code uses plain zlib here (DEFLATE only).
#pragma once
#include <zlib.h>
static inline int ZWRAP_isUsingZSTDcompression(void) { return 0; }
