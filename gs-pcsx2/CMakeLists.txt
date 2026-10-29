# PCSX2's GS (hardware renderer, OpenGL device) built as a library for the
# Switch, fed by ps2-web-recomp's GIF layer. PCSX2 is GPL-3.0, as un5-nx is;
# it stays out of the MIT ps2-web-recomp patches.
cmake_minimum_required(VERSION 3.20)
project(pcsx2gs CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PCSX2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party_src/pcsx2" CACHE PATH "PCSX2 source (sparse checkout with pcsx2/GS, common, 3rdparty/{fmt,glad,xxhash})")
set(GS "${PCSX2_SRC}/pcsx2/GS")

file(GLOB GS_CORE "${GS}/*.cpp")
# GS.cpp is PCSX2's front end to the GS (settings, hotkeys, the VM); src/ has ours.
list(FILTER GS_CORE EXCLUDE REGEX "/GS\\.cpp$|GSCapture|GSDump|GSLzma|GSPng")
file(GLOB GS_RENDERERS
    "${GS}/Renderers/Common/*.cpp"
    "${GS}/Renderers/HW/*.cpp"
    "${GS}/Renderers/Null/*.cpp"
    "${GS}/Renderers/OpenGL/GLContext.cpp"
    "${GS}/Renderers/OpenGL/GLContextEGL.cpp"
    "${GS}/Renderers/OpenGL/GLProgram.cpp"
    "${GS}/Renderers/OpenGL/GLShaderCache.cpp"
    "${GS}/Renderers/OpenGL/GLState.cpp"
    "${GS}/Renderers/OpenGL/GLStreamBuffer.cpp"
    "${GS}/Renderers/OpenGL/GSDeviceOGL.cpp"
    "${GS}/Renderers/OpenGL/GSTextureOGL.cpp")
list(FILTER GS_RENDERERS EXCLUDE REGEX "GSTextureReplacement|GSRendererHWMultiISA")

file(GLOB GS_GLUE "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
# PCSX2's common library, without what needs a desktop (HTTP, crash handler,
# YAML, image codecs) and with the Switch's DynamicLibrary in src/.
set(COMMON "${PCSX2_SRC}/common")
set(COMMON_SOURCES
    ${COMMON}/AlignedMalloc.cpp ${COMMON}/Assertions.cpp ${COMMON}/Console.cpp ${COMMON}/Error.cpp
    ${COMMON}/MD5Digest.cpp ${COMMON}/SmallString.cpp ${COMMON}/StringUtil.cpp
    ${COMMON}/Timer.cpp ${COMMON}/WindowInfo.cpp ${COMMON}/Perf.cpp
    ${COMMON}/ProgressCallback.cpp ${COMMON}/MemorySettingsInterface.cpp ${COMMON}/SettingsWrapper.cpp)
set(GLAD_SOURCES "${PCSX2_SRC}/3rdparty/glad/src/gl.c" "${PCSX2_SRC}/3rdparty/glad/src/egl.c")
# GS devices draw an ImGui overlay; with no ImGui context current they draw
# none. The in-game menu (src/ingame_menu.cpp) makes one while it is open.
set(IMGUI "${PCSX2_SRC}/3rdparty/imgui/src/imgui.cpp" "${PCSX2_SRC}/3rdparty/imgui/src/imgui_draw.cpp"
    "${PCSX2_SRC}/3rdparty/imgui/src/imgui_tables.cpp" "${PCSX2_SRC}/3rdparty/imgui/src/imgui_widgets.cpp"
    "${PCSX2_SRC}/3rdparty/imgui/src/imgui_freetype.cpp")
add_library(pcsx2gs STATIC ${GS_CORE} ${GS_RENDERERS} ${GS_GLUE} ${IMGUI} ${COMMON_SOURCES} ${GLAD_SOURCES})
# devkitPro's EGL headers know the Switch; glad's do not, so they come first.
find_path(SWITCH_EGL_INCLUDE EGL/eglplatform.h REQUIRED)
# The launcher draws text with FreeType (link it into ps2run: freetype,
# harfbuzz, bz2, png16, z, as the portlibs build of FreeType needs).
find_path(SWITCH_FREETYPE_INCLUDE ft2build.h PATH_SUFFIXES freetype2 REQUIRED)
target_include_directories(pcsx2gs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(pcsx2gs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${SWITCH_EGL_INCLUDE}" "${SWITCH_FREETYPE_INCLUDE}"
    "${PCSX2_SRC}" "${PCSX2_SRC}/pcsx2" "${GS}"
    "${PCSX2_SRC}/3rdparty/fmt/include" "${PCSX2_SRC}/3rdparty/glad/include" "${PCSX2_SRC}/3rdparty/include" "${PCSX2_SRC}/3rdparty/fast_float/include" "${PCSX2_SRC}/3rdparty/imgui/include")
target_compile_definitions(pcsx2gs PUBLIC ENABLE_OPENGL FMT_HEADER_ONLY)
# ImGui's default "open in shell" forks a process; the Switch has neither.
target_compile_definitions(pcsx2gs PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
# GCC refuses always_inline on functions it thinks the linker may interpose.
target_compile_options(pcsx2gs PRIVATE -Wno-deprecated-declarations -fno-semantic-interposition)

find_library(SWITCH_EGL EGL REQUIRED)
find_library(SWITCH_GLAPI glapi REQUIRED)
find_library(SWITCH_DRM_NOUVEAU drm_nouveau REQUIRED)
target_link_libraries(pcsx2gs PUBLIC ${SWITCH_EGL} ${SWITCH_GLAPI} ${SWITCH_DRM_NOUVEAU})

add_executable(gs_link_test test/link_test.cpp)
target_link_libraries(gs_link_test PRIVATE pcsx2gs)
