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

# libvu0.c is the one Sony library unit that is plain C arithmetic, with no hardware access, and the
# native build links it unchanged.
add_executable(
  resonance_host
  core/cycles.cpp
  core/heap.c
  core/libc.c
  core/main.cpp
  core/vumicrocode.S
  stubs/stubs.S
  stubs/unimplemented.c
  "${CMAKE_SOURCE_DIR}/sce/ee/src/libvu0.c")
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
