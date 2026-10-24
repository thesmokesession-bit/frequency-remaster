# libps2gfx: Play! (BSD-2, LICENSE.Play.txt) VIF1/VU1/GIF/GS behind a C API. No PS2VM/EE CPU/IOP.
set(P ${CMAKE_CURRENT_SOURCE_DIR}/vendor/play-src)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sources.txt PLAY_SOURCES)
list(TRANSFORM PLAY_SOURCES PREPEND ${P}/)

find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
find_package(Threads REQUIRED)

add_library(ps2gfx STATIC src/ps2gfx.cpp src/AppConfig.cpp src/TexReplace.cpp ${PLAY_SOURCES})
target_include_directories(ps2gfx PUBLIC include)
target_include_directories(ps2gfx PRIVATE src src/shim ${P}/Source ${P}/Source/app_shared ${P}/deps/Framework/include
	${P}/deps/CodeGen/include ${P}/deps/Dependencies/glew-2.0.0/include ${P}/deps/Dependencies/xxHash)
target_compile_definitions(ps2gfx PRIVATE GLEW_STATIC USE_GLEW NDEBUG)
target_compile_options(ps2gfx PRIVATE -msse -msse2 -mssse3 -w -g0)
set_target_properties(ps2gfx PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
target_link_libraries(ps2gfx PUBLIC OpenGL::GL ZLIB::ZLIB PNG::PNG Threads::Threads)

# Self-check of the HD texture key (src/TexReplaceCheck.cpp); built on request only.
add_executable(texreplace_check EXCLUDE_FROM_ALL src/TexReplaceCheck.cpp)
target_include_directories(texreplace_check PRIVATE src ${P}/Source ${P}/deps/Framework/include ${P}/deps/Dependencies/xxHash)
target_link_libraries(texreplace_check PRIVATE ps2gfx)
set_target_properties(texreplace_check PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
