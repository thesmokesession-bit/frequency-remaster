# Settings of the native build (RESONANCE_HOST). The game compiles with the build machine's
# compiler and the code generation choices of the Emotion Engine toolchain file. The executable is
# not position independent, so its static data sits below 4 GiB with the heap and the stacks, where
# a 32-bit DMA address can hold it.

# A PS5 title (cmake/ps5-toolchain.cmake) is always position independent, but the console loads it at
# 0x400000 as Linux loads the non-PIE executable, so its static data sits as low.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" OR NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR PS5))
  message(FATAL_ERROR "RESONANCE_HOST builds for x86-64 Linux or the PS5 only.")
endif()

add_compile_definitions(RESONANCE_HOST)
if(PS5)
  # host/ps5/include stands in for the glibc headers the console's C library lacks (<malloc.h>).
  include_directories(BEFORE "${CMAKE_SOURCE_DIR}/host/ps5/include")
  # Clang's flags of the same meaning. Line tables only: the title's unsigned ELF symbolizes crash
  # addresses, and full debug info would not fit the build machine's disk.
  add_compile_options(-O2 -gline-tables-only -fno-delete-null-pointer-checks -fwrapv
                      $<$<COMPILE_LANGUAGE:CXX>:-fno-sized-deallocation>)
else()
  add_compile_options(
    -O2
    -g
    -fno-pie
    -fno-isolate-erroneous-paths-dereference
    -fno-delete-null-pointer-checks
    -fwrapv
    $<$<COMPILE_LANGUAGE:CXX>:-fno-sized-deallocation>)
  add_link_options(-no-pie)
endif()
