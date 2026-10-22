# A port of no game, for CI: the host built and linked as a game's port is -
# main.cpp, the GAME options (the windowed entry, the disc picker's library,
# the version), the C runtime linked in - with a dispatch table of no
# functions (engine/tests/ci/stub_generated.py). It is built, never run.
# Everything else is engine/templates/game/port/CMakeLists.txt's.

cmake_minimum_required(VERSION 3.20)
project(ci_stub_port LANGUAGES CXX)
if(WIN32)
    enable_language(RC)     # the icon and version (engine/host/CMakeLists.txt)
endif()

set(PS2_GENERATED_DIR "" CACHE PATH "the stub's generated sources, from engine/tests/ci/stub_generated.py")

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    get_filename_component(repo_root ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)
    file(TO_NATIVE_PATH "${repo_root}/" repo_root_native)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options("/clang:-ffile-prefix-map=${repo_root}/=" "/clang:-ffile-prefix-map=${repo_root_native}="
                            "/clang:-ffp-contract=off")
    else()
        add_compile_options("/d1trimfile:${repo_root_native}")
    endif()
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../../host ${CMAKE_BINARY_DIR}/ps2host)

ps2_add_port(ci_stub
    GAME ${CMAKE_CURRENT_LIST_DIR}/..
    GENERATED ${PS2_GENERATED_DIR}
    TRANSLATED ${CMAKE_CURRENT_LIST_DIR}/../src/lifted
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/overrides.cpp)
