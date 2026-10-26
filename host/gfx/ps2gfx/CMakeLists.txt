# libps2gfx: Play! (BSD-2, LICENSE.Play.txt) VIF1/VU1/GIF/GS behind a C API. No PS2VM/EE CPU/IOP.
set(P ${CMAKE_CURRENT_SOURCE_DIR}/vendor/play-src)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sources.txt PLAY_SOURCES)
list(TRANSFORM PLAY_SOURCES PREPEND ${P}/)

# The PS5 has no OpenGL: the Vulkan GS on the static RADV driver draws there.
if(PS5)
	list(FILTER PLAY_SOURCES EXCLUDE REGEX "GSH_OpenGL|glew|/opengl/")
	set(PS2GFX_VULKAN ON CACHE BOOL "" FORCE)
else()
	find_package(OpenGL REQUIRED)
endif()
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
target_link_libraries(ps2gfx PUBLIC $<TARGET_NAME_IF_EXISTS:OpenGL::GL> ZLIB::ZLIB PNG::PNG Threads::Threads)
if(PS5)
	target_compile_definitions(ps2gfx PRIVATE PS2GFX_NO_OPENGL)
endif()

# The Vulkan GS (offscreen, GSH_VulkanOffscreen) for ports without OpenGL: needs only the Vulkan
# headers, as the loader is dlopen'ed at runtime. Off by default; the Linux host draws with OpenGL.
option(PS2GFX_VULKAN "Build the Vulkan GS handler" OFF)
if(PS2GFX_VULKAN)
	if(PS5)
		set(Vulkan_INCLUDE_DIRS "$ENV{RADV_INCLUDE_ROOT}/include") # RADV's own headers
	else()
		find_package(Vulkan REQUIRED)
	endif()
	file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sources_vulkan.txt VK_SOURCES)
	list(TRANSFORM VK_SOURCES PREPEND ${P}/)
	target_sources(ps2gfx PRIVATE ${VK_SOURCES})
	target_include_directories(ps2gfx PRIVATE ${P}/deps/Nuanceur/include ${Vulkan_INCLUDE_DIRS})
	target_compile_definitions(ps2gfx PRIVATE PS2GFX_VULKAN)
	target_link_libraries(ps2gfx PUBLIC ${CMAKE_DL_LIBS})
endif()

# Self-check of the HD texture key (src/TexReplaceCheck.cpp); built on request only.
add_executable(texreplace_check EXCLUDE_FROM_ALL src/TexReplaceCheck.cpp)
target_include_directories(texreplace_check PRIVATE src ${P}/Source ${P}/deps/Framework/include ${P}/deps/Dependencies/xxHash)
target_link_libraries(texreplace_check PRIVATE ps2gfx)
set_target_properties(texreplace_check PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
