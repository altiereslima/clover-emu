cmake_minimum_required(VERSION 3.20)
project(clover_emu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# dynarmic (ARM32 -> ARM64 JIT)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

find_package(SDL2 REQUIRED)

add_executable(clover-emu
    src/common.cpp
    src/memory.cpp
    src/thread.cpp
    src/signals.cpp
    src/loader.cpp
    src/vfs.cpp
    src/devices.cpp
    src/syscalls.cpp
    src/syscalls2.cpp
    src/hle.cpp
    src/hle_sdl.cpp
    src/hle_sdl2.cpp
    src/hle_gl.cpp
    src/hle_alsa.cpp
    src/display.cpp
    src/main.cpp
)
target_include_directories(clover-emu PRIVATE src)
target_link_libraries(clover-emu PRIVATE dynarmic SDL2::SDL2 "-framework OpenGL")
target_compile_options(clover-emu PRIVATE -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)
