# Settings of the native build (RESONANCE_HOST). The game compiles with the build machine's
# compiler and the code generation choices of the Emotion Engine toolchain file. The executable is
# not position independent, so its static data sits below 4 GiB with the heap and the stacks, where
# a 32-bit DMA address can hold it.

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "RESONANCE_HOST builds for x86-64 Linux only.")
endif()

add_compile_definitions(RESONANCE_HOST)
add_compile_options(
  -O2
  -g
  -fno-pie
  -fno-isolate-erroneous-paths-dereference
  -fno-delete-null-pointer-checks
  -fwrapv
  $<$<COMPILE_LANGUAGE:CXX>:-fno-sized-deallocation>)
add_link_options(-no-pie)
