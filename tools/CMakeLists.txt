cmake_minimum_required(VERSION 3.20)
project(Peggle2RecompTools CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/XenonRecomp xenonrecomp)

add_executable(xexdump xexdump/xexdump.cpp)
target_compile_definitions(xexdump PRIVATE NOMINMAX)
target_link_libraries(xexdump PRIVATE XenonUtils)

add_executable(jumptables jumptables/jumptables.cpp)
target_compile_definitions(jumptables PRIVATE NOMINMAX)
target_link_libraries(jumptables PRIVATE XenonUtils)
