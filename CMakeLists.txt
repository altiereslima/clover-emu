cmake_minimum_required(VERSION 3.20)
project(clover_emu C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# dynarmic needs Boost headers (icl, variant). On Windows scripts/setup-windows.ps1 fetches them.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/boost/icl/interval_set.hpp" AND NOT Boost_INCLUDE_DIR)
    set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost" CACHE PATH "" FORCE)
endif()

# dynarmic (ARM32 JIT -> ARM64 or x86-64)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

if (WIN32)
    # scripts/setup-windows.ps1 puts the official SDL2 VC package here
    if (NOT SDL2_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2-win/cmake")
        set(SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2-win/cmake")
    endif()
endif()
find_package(SDL2 REQUIRED)

set(CLOVER_SOURCES
    src/ui.cpp
    src/launcher.cpp
    src/lang_ptbr.cpp
    src/common.cpp
    src/memory.cpp
    src/thread.cpp
    src/signals.cpp
    src/loader.cpp
    src/vfs.cpp
    src/devices.cpp
    src/syscalls.cpp
    src/syscalls2.cpp
    src/ipc.cpp
    src/hash.cpp
    src/hle.cpp
    src/hle_sdl.cpp
    src/hle_sdl2.cpp
    src/hle_gl.cpp
    src/hle_alsa.cpp
    src/display.cpp
    src/main.cpp
)

if (WIN32)
    list(APPEND CLOVER_SOURCES src/platform_win.cpp src/hostfs_win.cpp src/gl_loader.cpp src/squashfs.cpp
         res/clover-emu.rc res/clover-emu.manifest)
else()
    list(APPEND CLOVER_SOURCES src/platform_posix.cpp src/hostfs_posix.cpp)
endif()

add_executable(clover-emu ${CLOVER_SOURCES})
target_include_directories(clover-emu PRIVATE src)
target_link_libraries(clover-emu PRIVATE dynarmic SDL2::SDL2)

if (WIN32)
    # Built-in squashfs extractor: zlib (SNES firmware) and XZ (NES firmware) decompression.
    include(FetchContent)
    FetchContent_Declare(zlib_src
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
    FetchContent_Declare(xz_embedded_src
        URL https://github.com/tukaani-project/xz-embedded/archive/refs/tags/v20240322.tar.gz)
    FetchContent_GetProperties(zlib_src)
    if (NOT zlib_src_POPULATED)
        FetchContent_Populate(zlib_src)
    endif()
    FetchContent_GetProperties(xz_embedded_src)
    if (NOT xz_embedded_src_POPULATED)
        FetchContent_Populate(xz_embedded_src)
    endif()
    add_library(clover_zlib STATIC
        ${zlib_src_SOURCE_DIR}/adler32.c ${zlib_src_SOURCE_DIR}/crc32.c ${zlib_src_SOURCE_DIR}/inffast.c
        ${zlib_src_SOURCE_DIR}/inflate.c ${zlib_src_SOURCE_DIR}/inftrees.c ${zlib_src_SOURCE_DIR}/zutil.c)
    target_include_directories(clover_zlib PUBLIC ${zlib_src_SOURCE_DIR})
    target_compile_definitions(clover_zlib PRIVATE _CRT_SECURE_NO_WARNINGS)
    set(XZD ${xz_embedded_src_SOURCE_DIR}/linux/lib/xz)
    add_library(clover_xz STATIC ${XZD}/xz_crc32.c ${XZD}/xz_crc64.c ${XZD}/xz_dec_stream.c ${XZD}/xz_dec_lzma2.c ${XZD}/xz_dec_bcj.c)
    target_include_directories(clover_xz PUBLIC ${xz_embedded_src_SOURCE_DIR}/linux/include/linux ${xz_embedded_src_SOURCE_DIR}/userspace)
    target_compile_definitions(clover_xz PUBLIC XZ_USE_CRC64 XZ_DEC_ANY_CHECK XZ_DEC_X86 XZ_DEC_ARM XZ_DEC_ARMTHUMB XZ_DEC_POWERPC XZ_DEC_IA64 XZ_DEC_SPARC)
    target_link_libraries(clover-emu PRIVATE clover_zlib clover_xz opengl32 winmm bcrypt ole32 uuid shell32)
    # GUI program: double-clicking it opens no console window. Started from a terminal it attaches
    # to that console instead (ui::init_console); main() stays the entry point.
    set_target_properties(clover-emu PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "Console Classics")
    if (MSVC)
        target_link_options(clover-emu PRIVATE /ENTRY:mainCRTStartup)
    endif()
    target_compile_definitions(clover-emu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    if (MSVC)
        target_compile_options(clover-emu PRIVATE /W3 /utf-8 /permissive- /Zc:__cplusplus /wd4244 /wd4267 /wd4146 /wd4018 /wd4996)
        # The guest is a real-time console: optimise even in RelWithDebInfo.
        set_property(TARGET clover-emu PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    # SDL2.dll next to the executable
    add_custom_command(TARGET clover-emu POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:clover-emu>)
elseif (APPLE)
    target_link_libraries(clover-emu PRIVATE "-framework OpenGL")
    target_compile_options(clover-emu PRIVATE -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(clover-emu PRIVATE OpenGL::GL)
    target_compile_options(clover-emu PRIVATE -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)
endif()
