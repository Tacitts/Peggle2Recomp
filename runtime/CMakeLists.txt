cmake_minimum_required(VERSION 3.20)
project(Peggle2 CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# XenonUtils (XEX loading) and the recompiled guest code library.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/XenonRecomp xenonrecomp EXCLUDE_FROM_ALL)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib guestlib)

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
    net.cpp
    input.cpp
    render.cpp
    generated/import_stubs.cpp
)
target_include_directories(Peggle2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(Peggle2 PRIVATE NOMINMAX)
target_compile_options(Peggle2 PRIVATE /clang:-march=sandybridge -Wno-unused-parameter /Z7)
target_link_options(Peggle2 PRIVATE /DEBUG)
target_link_libraries(Peggle2 PRIVATE Peggle2Recomp XenonUtils ws2_32 dbghelp xinput)
