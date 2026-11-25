cmake_minimum_required(VERSION 3.20)
project(Peggle2Recomp CXX)

# Static library of the recompiled guest code (recomp/ppc). The runtime/executable
# that links against it does not exist yet; this target proves the code compiles.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ppc)
set(SIMDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/XenonRecomp/thirdparty/simde)

file(GLOB PPC_SOURCES ${PPC_DIR}/ppc_recomp.*.cpp ${PPC_DIR}/ppc_func_mapping.cpp)

add_library(Peggle2Recomp STATIC ${PPC_SOURCES})
target_include_directories(Peggle2Recomp PUBLIC ${PPC_DIR} ${SIMDE_DIR})
target_compile_options(Peggle2Recomp PRIVATE
    /clang:-march=sandybridge
    /clang:-fno-strict-aliasing
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    # Line tables only: lets the crash reporter map a host address back to the generated
    # source line, whose comment carries the guest instruction address.
    /clang:-gline-tables-only
    /Z7
)
