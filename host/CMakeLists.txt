# The native executable. It links every game archive the Emotion Engine build links, the Sony MPEG
# sample, and host/ in place of the Sony libraries, the runtime, and the start-up code under sce/ee.

# host/core/include comes first, so that a header there can adjust the Sony header it wraps.
target_include_directories(resonance_common BEFORE
                           INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/core/include")

# The game's main() is a routine host/core/main.cpp calls on the game's thread.
target_compile_definitions(resonance_main PRIVATE main=GameMain)

set(RESONANCE_EZMPEG_SAMPLE_DIR "${CMAKE_SOURCE_DIR}/sce/ee/sample")
add_library(
  resonance_ezmpeg STATIC
  "${RESONANCE_EZMPEG_SAMPLE_DIR}/audiodec.c"
  "${RESONANCE_EZMPEG_SAMPLE_DIR}/disp.c"
  "${RESONANCE_EZMPEG_SAMPLE_DIR}/readbuf.c"
  "${RESONANCE_EZMPEG_SAMPLE_DIR}/strfile.c"
  "${RESONANCE_EZMPEG_SAMPLE_DIR}/vibuf.c"
  "${RESONANCE_EZMPEG_SAMPLE_DIR}/videodec.c"
  "${RESONANCE_EZMPEG_SAMPLE_DIR}/vobuf.c")
target_link_libraries(resonance_ezmpeg PRIVATE resonance_common)
# The movie's IOP buffers come from host/mpeg/mpeg.cpp; its sound driver calls go to
# host/audio/audio.cpp's sceSdRemote(), which plays its PCM.
set_source_files_properties(
  "${RESONANCE_EZMPEG_SAMPLE_DIR}/audiodec.c"
  PROPERTIES COMPILE_DEFINITIONS
             "sceSifAllocIopHeap=HostMovieAllocIop;sceSifFreeIopHeap=HostMovieFreeIop")

# The Sony library units that are plain C with no hardware access (the VU0 arithmetic, the packet
# builders, and the MIDI input stream) link unchanged.
add_executable(
  resonance_host
  core/cycles.cpp
  core/heap.c
  core/libc.c
  core/main.cpp
  core/progress.cpp
  core/settings.c
  core/vumicrocode.S
  gfx-null/gfx.cpp
  gfx-null/libgraph.c
  io/disc.cpp
  io/memcard.cpp
  io/pad.cpp
  io/stdio.cpp
  kernel/iop.cpp
  kernel/kernel.cpp
  mpeg/mpeg.cpp
  net/netgame.cpp
  net/online.cpp
  stubs/stubs.S
  stubs/unimplemented.c
  "${CMAKE_SOURCE_DIR}/sce/ee/src/libgifpk.c"
  "${CMAKE_SOURCE_DIR}/sce/ee/src/libvifpk.c"
  "${CMAKE_SOURCE_DIR}/sce/ee/src/libvu0.c"
  "${CMAKE_SOURCE_DIR}/sce/ee/src/msin.c")
# dlmalloc is compiled as released.
set_source_files_properties(core/heap.c PROPERTIES COMPILE_OPTIONS -w)
set_source_files_properties(
  core/vumicrocode.S
  PROPERTIES COMPILE_OPTIONS "-Wa,-I${CMAKE_CURRENT_SOURCE_DIR}/core"
             OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/core/vu0microcode.bin;\
${CMAKE_CURRENT_SOURCE_DIR}/core/vu1microcode.bin")

# The metagame screens and the script interface link whole, as in the Emotion Engine build.
set(RESONANCE_HOST_ARCHIVES ${RESONANCE_LIBRARIES})
list(REMOVE_ITEM RESONANCE_HOST_ARCHIVES resonance_met resonance_script)
target_link_libraries(
  resonance_host
  PRIVATE resonance_common
          -Wl,--whole-archive
          resonance_met
          resonance_script
          -Wl,--no-whole-archive
          -Wl,--start-group
          ${RESONANCE_HOST_ARCHIVES}
          resonance_ezmpeg
          -Wl,--end-group)
# host/stubs/gen-stubs.py reads the link errors by their symbol names.
target_link_options(resonance_host PRIVATE -Wl,--no-demangle)

# host/mpeg decodes the movies with libavcodec (FFmpeg, LGPL), linked dynamically.
if(PS5)
  # The PS5 title: PacBrew's static FFmpeg and what it needs (host/ps5/link-title.sh finds each
  # -l name in the PacBrew sysroot) and the platform layer.
  target_sources(resonance_host PRIVATE ps5/ps5.c)
  target_link_libraries(resonance_host PRIVATE avcodec swresample avutil x264 lzma curl psl ssl crypto
                        zstd z)
  # The updater (ps5/ps5.c) names the commit a build is from; host/ps5/build.sh passes it.
  set(FREQ_VERSION "" CACHE STRING "The commit the PS5 build is made from, for its updater")
  if(FREQ_VERSION)
    set_source_files_properties(ps5/ps5.c PROPERTIES COMPILE_DEFINITIONS FREQ_VERSION="${FREQ_VERSION}")
  endif()
  # The game's gzip inflate() apart from zlib's, which the RADV archive links whole.
  target_compile_definitions(resonance_os PRIVATE inflate=GzipInflate)
  set_property(
    TARGET resonance_host
    APPEND
    PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ps5/link-title.sh")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec libavutil)
  target_link_libraries(resonance_host PRIVATE PkgConfig::LIBAVCODEC)
  # Online play (host/net): the signalling server over HTTPS, and the datagrams' tags.
  pkg_check_modules(ONLINE REQUIRED IMPORTED_TARGET libcurl libcrypto)
  target_link_libraries(resonance_host PRIVATE PkgConfig::ONLINE)
endif()

# The OpenGL backend in host/gfx draws through libps2gfx (Play!, BSD-2) in an SDL2 window.
find_package(SDL2 QUIET)
option(RESONANCE_PS2GFX "Draw with host/gfx (OpenGL and SDL2) instead of the null backend"
       ${SDL2_FOUND})
if(RESONANCE_PS2GFX)
  find_package(SDL2 REQUIRED)
  add_subdirectory(gfx/ps2gfx)
  target_sources(resonance_host PRIVATE gfx/gfx.cpp)
  target_link_libraries(resonance_host PRIVATE ps2gfx SDL2::SDL2)
  # host/gfx/gfx.cpp anchors its screenshots at the breadcrumbs of HostBreadcrumb(const char *).
  target_link_options(
    resonance_host PRIVATE -Wl,--wrap=_Z14HostBreadcrumbPKc
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/gfx/local-symbols.map")
  set_property(
    TARGET resonance_host
    APPEND
    PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gfx/local-symbols.map")
endif()

# Sound: the game's own driver on Play!'s SPU2, out through SDL2 (host/audio). It needs SDL2 and the
# Framework libps2gfx vendors, whichever backend draws.
find_package(SDL2 REQUIRED)
if(NOT TARGET ps2gfx)
  add_subdirectory(gfx/ps2gfx)
endif()
add_subdirectory(audio)
target_link_libraries(resonance_host PRIVATE resonance_audio)
