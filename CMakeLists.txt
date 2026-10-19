cmake_minimum_required(VERSION 3.20)
project(bonkheads C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The game code is recompiled at build time from YOUR copy of the original
# archive; nothing derived from the original program is stored in this repo.
set(BONKHEADS_SIT_DEFAULT "${CMAKE_SOURCE_DIR}/Bonkheads_Deluxe.SIT")
set(BONKHEADS_SIT "${BONKHEADS_SIT_DEFAULT}" CACHE FILEPATH
    "Path to the original Bonkheads_Deluxe.SIT (StuffIt 5) archive")
# Build the port (runtime, Toolbox, platform) against an empty game image
# instead of recompiling the archive. The result links and runs --help but
# cannot play; it lets CI check the port sources without the copyrighted game.
option(BONKHEADS_STUB_GAME "Link against an empty stub instead of the recompiled game" OFF)
option(BONKHEADS_WERROR "Treat warnings in the port sources as errors" OFF)

if(NOT BONKHEADS_STUB_GAME)
    # Unless a path was given, accept the archive under any name in the
    # repository root.
    if(BONKHEADS_SIT STREQUAL BONKHEADS_SIT_DEFAULT AND NOT EXISTS "${BONKHEADS_SIT}")
        file(GLOB SIT_CANDIDATES "${CMAKE_SOURCE_DIR}/*.sit" "${CMAKE_SOURCE_DIR}/*.SIT")
        list(REMOVE_DUPLICATES SIT_CANDIDATES)
        list(LENGTH SIT_CANDIDATES SIT_COUNT)
        if(SIT_COUNT EQUAL 1)
            set(BONKHEADS_SIT "${SIT_CANDIDATES}" CACHE FILEPATH "" FORCE)
        endif()
    endif()
    if(NOT EXISTS "${BONKHEADS_SIT}")
        message(FATAL_ERROR
            "Game archive not found: ${BONKHEADS_SIT}\n"
            "This repository contains no game data. Copy your own Bonkheads_Deluxe.SIT "
            "into ${CMAKE_SOURCE_DIR}, or pass -DBONKHEADS_SIT=/path/to/Bonkheads_Deluxe.SIT. "
            "To build only the engine without the game, pass -DBONKHEADS_STUB_GAME=ON.")
    endif()
    message(STATUS "Game archive: ${BONKHEADS_SIT}")
    find_package(Python3 3.8 COMPONENTS Interpreter)
    if(NOT Python3_FOUND)
        message(FATAL_ERROR
            "Python 3.8 or newer is required to extract and recompile the game. "
            "Install it from https://www.python.org/downloads/ (on Windows, tick "
            "\"Add python.exe to PATH\") and run the build again.")
    endif()
endif()

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.10.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL3)

# stb_truetype (public domain), pinned
set(STB_COMMIT 2c980bb59875b0d32144a71867fbdebb2f77cd20)
set(STB_DIR "${CMAKE_BINARY_DIR}/third_party/stb")
if(NOT EXISTS "${STB_DIR}/stb_truetype.h")
    file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}/stb_truetype.h"
         "${STB_DIR}/stb_truetype.h" STATUS STB_STATUS)
    list(GET STB_STATUS 0 STB_CODE)
    if(NOT STB_CODE EQUAL 0)
        message(FATAL_ERROR "failed to download stb_truetype.h: ${STB_STATUS}")
    endif()
endif()

set(DATA_DIR "${CMAKE_BINARY_DIR}/data")
if(BONKHEADS_STUB_GAME)
    set(GEN_DIR "${CMAKE_SOURCE_DIR}/tools/stub")
    set(GEN_SOURCES ${GEN_DIR}/recomp_table.c)
else()
    set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
    set(APP_RSRC "${DATA_DIR}/Bonkheads Deluxe/Bonkheads Deluxe.rsrc")
    set(GEN_SOURCES
        ${GEN_DIR}/recomp_seg1.c ${GEN_DIR}/recomp_seg2.c ${GEN_DIR}/recomp_seg3.c
        ${GEN_DIR}/recomp_seg4.c ${GEN_DIR}/recomp_seg5.c ${GEN_DIR}/recomp_seg7.c
        ${GEN_DIR}/recomp_table.c)

    file(GLOB TOOL_SCRIPTS ${CMAKE_SOURCE_DIR}/tools/*.py)

    add_custom_command(
        OUTPUT "${APP_RSRC}"
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/sit5.py --extract "${BONKHEADS_SIT}" "${DATA_DIR}"
        DEPENDS "${BONKHEADS_SIT}" ${CMAKE_SOURCE_DIR}/tools/sit5.py
        COMMENT "Extracting original game archive"
        VERBATIM)

    add_custom_command(
        OUTPUT ${GEN_SOURCES} ${GEN_DIR}/recomp_funcs.h
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recomp.py "${APP_RSRC}" "${GEN_DIR}"
        DEPENDS "${APP_RSRC}" ${TOOL_SCRIPTS}
        COMMENT "Recompiling 68k code to C"
        VERBATIM)
endif()

file(GLOB_RECURSE PORT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.c)

add_executable(bonkheads ${PORT_SOURCES} ${GEN_SOURCES})
target_include_directories(bonkheads PRIVATE ${CMAKE_SOURCE_DIR}/src ${GEN_DIR} ${STB_DIR})
target_link_libraries(bonkheads PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(bonkheads PRIVATE ws2_32 dbghelp)
    set_target_properties(bonkheads PROPERTIES WIN32_EXECUTABLE OFF)
endif()
if(MSVC)
    target_compile_options(bonkheads PRIVATE /W3 /wd4996 /bigobj $<$<BOOL:${BONKHEADS_WERROR}>:/WX>)
    if(NOT BONKHEADS_STUB_GAME)
        set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")
    endif()
else()
    target_compile_options(bonkheads PRIVATE -Wall -Wno-unused-label -Wno-unused-variable
        $<$<BOOL:${BONKHEADS_WERROR}>:-Werror>)
    if(NOT BONKHEADS_STUB_GAME)
        set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    target_link_libraries(bonkheads PRIVATE m)
endif()

# Ship the extracted resource forks next to the executable (a no-op when the
# build folder is the executable's folder, as with Ninja and Makefiles).
if(NOT BONKHEADS_STUB_GAME)
    add_custom_command(TARGET bonkheads POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${DATA_DIR}" "$<TARGET_FILE_DIR:bonkheads>/data"
        VERBATIM)
endif()
