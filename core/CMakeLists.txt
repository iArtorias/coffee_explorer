cmake_minimum_required(VERSION 3.25)

project(pecore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Debug builds only.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:Embedded>")
set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)

option(PECORE_WITH_ZYDIS "Fetch Zydis for the disassembler" ON)
set(PECORE_PELIB_DIR "" CACHE PATH "Local PeLib source folder, empty fetches iArtorias/pelib-updated from GitHub")

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Every linked target of this folder and below (pecore, the shell extension): no debug linkage outside Debug
    add_link_options("$<$<NOT:$<CONFIG:Debug>>:/DEBUG:NONE>")
endif()

include(FetchContent)

# PeLib (updated RetDec fork), fetched at a fixed commit unless 'PECORE_PELIB_DIR' names a local copy. Its own CMake files
# define the same target twice, so only the sources are fetched ('SOURCE_SUBDIR' without a 'CMakeLists.txt') and listed here.
if(NOT PECORE_PELIB_DIR)
    FetchContent_Declare(pelib
        GIT_REPOSITORY https://github.com/iArtorias/pelib-updated.git
        GIT_TAG 4b68ae2c4a8fe07cb9b35e627bae0d21cfde0445
        GIT_SHALLOW FALSE
        SOURCE_SUBDIR do_not_configure)
    FetchContent_MakeAvailable(pelib)
    set(PELIB_SOURCE_DIR "${pelib_SOURCE_DIR}")
else()
    set(PELIB_SOURCE_DIR "${PECORE_PELIB_DIR}")
endif()

if(NOT EXISTS "${PELIB_SOURCE_DIR}/include/retdec/pelib/PeFile.h")
    message(FATAL_ERROR "PeLib not found in ${PELIB_SOURCE_DIR}, clear PECORE_PELIB_DIR to fetch it or point it at a pelib-updated checkout")
endif()

file(GLOB PELIB_SOURCES "${PELIB_SOURCE_DIR}/src/pelib/*.cpp" "${PELIB_SOURCE_DIR}/src/utils/*.cpp")
add_library(pelib STATIC ${PELIB_SOURCES})
target_include_directories(pelib SYSTEM PUBLIC "${PELIB_SOURCE_DIR}/include")
set_target_properties(pelib PROPERTIES CXX_STANDARD 17)

if(MSVC)
    target_compile_options(pelib PRIVATE /w /EHsc /utf-8)
else()
    # PeLib relies on MSVC pulling <cstdint> in transitively
    target_compile_options(pelib PRIVATE -w -include cstdint)
endif()

# BLAKE3 ('third_party/blake3', unchanged). MSVC x64 builds the SIMD kernels with ml64, other toolchains use the
# portable code.
set(BLAKE3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/blake3")
add_library(blake3 STATIC "${BLAKE3_DIR}/blake3.c" "${BLAKE3_DIR}/blake3_dispatch.c" "${BLAKE3_DIR}/blake3_portable.c")
target_include_directories(blake3 SYSTEM PUBLIC "${BLAKE3_DIR}")

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    enable_language(ASM_MASM)
    target_sources(blake3 PRIVATE
        "${BLAKE3_DIR}/blake3_sse2_x86-64_windows_msvc.asm"
        "${BLAKE3_DIR}/blake3_sse41_x86-64_windows_msvc.asm"
        "${BLAKE3_DIR}/blake3_avx2_x86-64_windows_msvc.asm"
        "${BLAKE3_DIR}/blake3_avx512_x86-64_windows_msvc.asm")
    target_compile_options(blake3 PRIVATE $<$<COMPILE_LANGUAGE:C>:/w>)
else()
    target_compile_definitions(blake3 PRIVATE BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
    target_compile_options(blake3 PRIVATE -w)
endif()

if(PECORE_WITH_ZYDIS)
    include(FetchContent)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zydis
        GIT_REPOSITORY https://github.com/zyantific/zydis.git
        GIT_TAG v4.1.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(zydis)
endif()

set(PECORE_SOURCES
    src/Types.cpp
    src/Json.cpp
    src/Hash.cpp
    src/Authenticode.cpp
    src/Document.cpp
    src/PeImage.cpp
    src/Meta.cpp
    src/ViewBuilder.cpp
    src/HeaderViews.cpp
    src/RichHeader.cpp
    src/DirectoryViews.cpp
    src/DotNet.cpp
    src/Resources.cpp
    src/Operations.cpp
    src/Symbols.cpp
    src/Tools.cpp
    src/Platform.cpp
    src/Api.cpp
)

# Shared library for the UI.
add_library(pecore SHARED ${PECORE_SOURCES})
target_include_directories(pecore PUBLIC include PRIVATE src)
target_compile_definitions(pecore PRIVATE PECORE_EXPORTS)
target_link_libraries(pecore PRIVATE pelib blake3)
set_target_properties(pecore PROPERTIES PREFIX "")

foreach(TARGET_NAME pecore)
    if(PECORE_WITH_ZYDIS)
        target_link_libraries(${TARGET_NAME} PRIVATE Zydis)
        target_compile_definitions(${TARGET_NAME} PRIVATE PECORE_WITH_ZYDIS)
    endif()

    if(WIN32)
        target_compile_definitions(${TARGET_NAME} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
        target_link_libraries(${TARGET_NAME} PRIVATE wintrust dbghelp bcrypt)
    endif()

    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
    else()
        target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endforeach()

if(MINGW)
    target_link_options(pecore PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# 'pecore.pdb' next to 'pecore.dll' in Debug builds only, Release links without any debug information.
if(MSVC)
    target_link_options(pecore PRIVATE "$<$<CONFIG:Debug>:/DEBUG>" /OPT:REF /OPT:ICF)
endif()

# Windows 11 context menu handler of the UI, next to 'pecore.dll' in the output.
if(MSVC)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shell" "${CMAKE_CURRENT_BINARY_DIR}/shell")
endif()