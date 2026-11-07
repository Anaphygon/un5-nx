# PCSX2's GS with the Switch's deko3d device, built for the Mac with nothing
# behind deko3d (src/host/dk_stub.cpp): the shader harvest. Linked into the
# Mac runner as its external GS, it plays the game headless and keeps the
# source of every shader the Switch would need (tools/dk_shaders.py builds
# them with uam). GPL-3.0, as the Switch library.
cmake_minimum_required(VERSION 3.20)
project(pcsx2gs_host CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(PCSX2_SRC "${ROOT}/../../third_party_src/pcsx2" CACHE PATH "PCSX2 source (the Switch build's sparse checkout)")
set(DEKO3D_INCLUDE "/opt/devkitpro/libnx/include" CACHE PATH "Where deko3d.h is")
set(GS "${PCSX2_SRC}/pcsx2/GS")

file(GLOB GS_CORE "${GS}/*.cpp")
list(FILTER GS_CORE EXCLUDE REGEX "/GS\\.cpp$|GSCapture|GSDump|GSLzma|GSPng")
file(GLOB GS_RENDERERS "${GS}/Renderers/Common/*.cpp" "${GS}/Renderers/HW/*.cpp" "${GS}/Renderers/Null/*.cpp")
list(FILTER GS_RENDERERS EXCLUDE REGEX "GSTextureReplacement|GSRendererHWMultiISA")
# The front end without the Switch's launcher, window, clocks and OpenGL bits.
set(FRONT
    ${ROOT}/src/gs_front.cpp ${ROOT}/src/pcsx2_glue.cpp ${ROOT}/src/emu_config_storage.cpp ${ROOT}/src/hw_no_sw.cpp
    ${ROOT}/src/bench.cpp ${ROOT}/src/wrapped_memory_switch.cpp
    ${ROOT}/src/deko3d/GSDeviceDK.cpp ${ROOT}/src/deko3d/GSTextureDK.cpp ${ROOT}/src/deko3d/dk_context.cpp
    ${ROOT}/src/host/dk_stub.cpp ${ROOT}/src/host/host_platform.cpp ${ROOT}/src/host/host_stubs.cpp)
set(COMMON "${PCSX2_SRC}/common")
set(COMMON_SOURCES
    ${COMMON}/AlignedMalloc.cpp ${COMMON}/Assertions.cpp ${COMMON}/Console.cpp ${COMMON}/Error.cpp
    ${COMMON}/MD5Digest.cpp ${COMMON}/SmallString.cpp ${COMMON}/StringUtil.cpp
    ${COMMON}/Timer.cpp ${COMMON}/WindowInfo.cpp ${COMMON}/Perf.cpp
    ${COMMON}/ProgressCallback.cpp ${COMMON}/MemorySettingsInterface.cpp ${COMMON}/SettingsWrapper.cpp)
set(IMGUI "${PCSX2_SRC}/3rdparty/imgui/src/imgui.cpp" "${PCSX2_SRC}/3rdparty/imgui/src/imgui_draw.cpp"
    "${PCSX2_SRC}/3rdparty/imgui/src/imgui_tables.cpp" "${PCSX2_SRC}/3rdparty/imgui/src/imgui_widgets.cpp"
    "${PCSX2_SRC}/3rdparty/imgui/src/imgui_freetype.cpp")
find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
add_library(pcsx2gs_host STATIC ${GS_CORE} ${GS_RENDERERS} ${FRONT} ${IMGUI} ${COMMON_SOURCES})
target_include_directories(pcsx2gs_host PUBLIC "${ROOT}/include")
target_include_directories(pcsx2gs_host PRIVATE
    "${ROOT}/src" "${ROOT}/shim" "${PCSX2_SRC}" "${PCSX2_SRC}/pcsx2" "${GS}"
    "${PCSX2_SRC}/3rdparty/fmt/include" "${PCSX2_SRC}/3rdparty/include" "${PCSX2_SRC}/3rdparty/fast_float/include"
    "${PCSX2_SRC}/3rdparty/imgui/include")
# deko3d.h alone (it needs nothing else from libnx).
target_include_directories(pcsx2gs_host SYSTEM PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/deko3d")
file(COPY "${DEKO3D_INCLUDE}/deko3d.h" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/deko3d")
target_compile_definitions(pcsx2gs_host PUBLIC FMT_HEADER_ONLY UN5_HOST)
target_compile_definitions(pcsx2gs_host PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_compile_options(pcsx2gs_host PRIVATE -Wno-deprecated-declarations)
target_link_libraries(pcsx2gs_host PUBLIC PkgConfig::FREETYPE)
