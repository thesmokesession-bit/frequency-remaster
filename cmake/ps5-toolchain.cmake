# The native build as a PS5 title (RESONANCE_HOST=ON): Mihawk's payload SDK fork with SDL2 and
# FFmpeg from the PacBrew sysroot and the static RADV driver, as Perfect Eye builds Perfect Dark.
# host/ps5/build.sh sets PS5_PAYLOAD_SDK, PACBREW, RADV_ROOT and RADV_INCLUDE_ROOT.
include("$ENV{PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
list(APPEND CMAKE_FIND_ROOT_PATH "$ENV{PACBREW}" "$ENV{RADV_INCLUDE_ROOT}")
list(APPEND CMAKE_PREFIX_PATH "$ENV{PACBREW}" "$ENV{RADV_INCLUDE_ROOT}")
set(CMAKE_VERBOSE_MAKEFILE OFF CACHE BOOL "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# OPENSSL_ROOT's OpenSSL comes before the sysroot's older one (host/ps5/build.sh).
set(_ps5_includes "-isystem $ENV{OPENSSL_ROOT}/include -isystem $ENV{PACBREW}/include/SDL2")
string(APPEND _ps5_includes " -isystem $ENV{PACBREW}/include")
set(CMAKE_C_FLAGS_INIT "${_ps5_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_ps5_includes}")
# The executable is linked into a title by host/ps5/link-title.sh (static RADV, signing).
set(CMAKE_C_LINK_EXECUTABLE "bash ${CMAKE_CURRENT_LIST_DIR}/../host/ps5/link-title.sh <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "bash ${CMAKE_CURRENT_LIST_DIR}/../host/ps5/link-title.sh <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES>")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
