cmake_minimum_required(VERSION 3.20)
project(Peggle2 CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# XenonUtils (XEX loading) and the recompiled guest code library.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/XenonRecomp xenonrecomp EXCLUDE_FROM_ALL)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib guestlib)

# XenosRecomp's shader translator, built into the runtime to translate draw shaders on the fly.
set(XENOS ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/XenosRecomp)
add_library(XenosShaderRecompiler STATIC ${XENOS}/XenosRecomp/shader_recompiler.cpp)
target_include_directories(XenosShaderRecompiler PUBLIC
    ${XENOS}/XenosRecomp
    ${XENOS}/thirdparty/fmt/include
    ${XENOS}/thirdparty/dxc-bin/inc
    ${XENOS}/thirdparty/smol-v/source
    ${XENOS}/thirdparty/xxHash
    ${XENOS}/thirdparty/zstd/lib)
target_compile_definitions(XenosShaderRecompiler PUBLIC FMT_HEADER_ONLY NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(XenosShaderRecompiler PRIVATE /FI${XENOS}/XenosRecomp/pch.h -Wno-switch -Wno-unused-variable)

# shader_common.h as a string literal for the translator (shader_common_text.inc).
file(READ ${XENOS}/XenosRecomp/shader_common.h SHADER_COMMON_TEXT)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/shader_common_text.inc "R\"__XENOS__(${SHADER_COMMON_TEXT})__XENOS__\"\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${XENOS}/XenosRecomp/shader_common.h)

add_executable(Peggle2
    main.cpp
    memory.cpp
    kernel.cpp
    fs.cpp
    threads.cpp
    gpu.cpp
    audio.cpp
    hooks.cpp
    xam.cpp
    content.cpp
    xma.cpp
    xmsg.cpp
    net.cpp
    input.cpp
    render.cpp
    draw.cpp
    xenos_shader.cpp
    generated/import_stubs.cpp
)
target_include_directories(Peggle2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(Peggle2 PRIVATE NOMINMAX)
target_compile_options(Peggle2 PRIVATE /clang:-march=sandybridge -Wno-unused-parameter /Z7)
set_source_files_properties(xenos_shader.cpp PROPERTIES COMPILE_OPTIONS "/FI${XENOS}/XenosRecomp/pch.h;-Wno-switch")
target_link_options(Peggle2 PRIVATE /DEBUG)
target_link_libraries(Peggle2 PRIVATE Peggle2Recomp XenonUtils XenosShaderRecompiler ws2_32 dbghelp xinput winmm)

# Optional: FFmpeg's libavcodec for XMA audio (xma.cpp), from vcpkg at ../vcpkg next to the repo:
#   vcpkg install "ffmpeg[core,avcodec]:x64-windows-static"
# Without it the build still works; XMA music and effects stay silent.
set(PEGGLE2_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../vcpkg/installed/x64-windows-static" CACHE PATH "vcpkg installed tree with FFmpeg")
if (EXISTS "${PEGGLE2_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    target_compile_definitions(Peggle2 PRIVATE PEGGLE2_HAVE_FFMPEG)
    target_include_directories(Peggle2 PRIVATE ${PEGGLE2_FFMPEG_ROOT}/include)
    target_link_libraries(Peggle2 PRIVATE ${PEGGLE2_FFMPEG_ROOT}/lib/avcodec.lib ${PEGGLE2_FFMPEG_ROOT}/lib/avutil.lib
        bcrypt mfuuid strmiids ole32 user32)
    message(STATUS "FFmpeg found in ${PEGGLE2_FFMPEG_ROOT}: XMA audio enabled")
else()
    message(STATUS "FFmpeg not found (${PEGGLE2_FFMPEG_ROOT}): XMA audio disabled")
endif()

# DXC runtime (loaded by xenos_shader.cpp) next to the executable.
add_custom_command(TARGET Peggle2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XENOS}/thirdparty/dxc-bin/bin/x64/dxcompiler.dll $<TARGET_FILE_DIR:Peggle2>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XENOS}/thirdparty/dxc-bin/bin/x64/dxil.dll $<TARGET_FILE_DIR:Peggle2>)
