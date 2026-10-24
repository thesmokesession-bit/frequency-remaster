# Sound: the game's IOP sound driver (src/ezmidi) built for the host, Sony's libsd on Play!'s SPU2
# (BSD-2, play/LICENSE.Play.txt), and the output (audio.cpp). Play!'s SPU2 uses the Framework that
# libps2gfx vendors, and its save-state code links against the copy in ps2gfx.
set(PS2GFX_PLAY ${CMAKE_CURRENT_SOURCE_DIR}/../gfx/ps2gfx/vendor/play-src)
add_library(
  playspu2 STATIC
  play/Source/iop/Iop_SpuBase.cpp play/Source/iop/Iop_Spu2.cpp play/Source/iop/Iop_Spu2_Core.cpp
  play/Source/states/RegisterStateCollectionFile.cpp)
target_include_directories(
  playspu2 PUBLIC play/Source/iop play/Source play/deps/Framework/include
                  ${PS2GFX_PLAY}/deps/Framework/include
  PRIVATE ${PS2GFX_PLAY}/Source ${PS2GFX_PLAY}/Source/app_shared)
target_compile_definitions(playspu2 PRIVATE NDEBUG)
target_compile_options(playspu2 PRIVATE -w)
set_target_properties(playspu2 PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
target_link_libraries(playspu2 PUBLIC ps2gfx)

# The driver against the IOP headers, with the IOP calls renamed apart from the EE's.
add_library(ezmidi_host STATIC ${CMAKE_SOURCE_DIR}/src/ezmidi/midi_com.c
                               ${CMAKE_SOURCE_DIR}/src/ezmidi/midi_hsyn.c)
target_include_directories(ezmidi_host PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/sce/iop/include)
target_compile_options(ezmidi_host PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/iop-names.h
                                           -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast)

add_library(resonance_audio STATIC audio.cpp)
target_link_libraries(resonance_audio PRIVATE resonance_common playspu2 ezmidi_host SDL2::SDL2)
set_target_properties(resonance_audio PROPERTIES CXX_STANDARD 17)
