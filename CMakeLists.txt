cmake_minimum_required(VERSION 3.16)

# Read version from file
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" YTCUI_VERSION_STR LIMIT_COUNT 1)
string(STRIP "${YTCUI_VERSION_STR}" YTCUI_VERSION_STR)

project(ytcui VERSION ${YTCUI_VERSION_STR} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Reject accidental C++20 syntax while retaining supported C++17 toolchains.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-Werror=c++20-extensions" YTCUI_HAS_CXX20_EXTENSION_WARNING)
if(YTCUI_HAS_CXX20_EXTENSION_WARNING)
    add_compile_options(-Werror=c++20-extensions)
endif()
# Preserve pkgsrc/Homebrew/ports dependency directories after installation.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
option(YTCUI_SANITIZE "Enable AddressSanitizer for runtime regression tests" OFF)
if(YTCUI_SANITIZE)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# ── Find dependencies ─────────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE    TRUE)
find_package(Curses REQUIRED)

# ── Sources ───────────────────────────────────────────────────────────────────

# v4 ncurses app sources
set(APP_SRCS
    src/main.cpp
    src/app.cpp
    src/config.cpp
    src/input.cpp
    src/mpv_ipc.cpp
    src/player.cpp
    src/player_we.cpp
    src/player_resolve.cpp
    src/termcaps.cpp
    src/tui.cpp
    src/youtube.cpp
)

# v5 portable core modules — no FTXUI, no v5 config/keymap deps
# backend_*.cpp and src/core/config.cpp are excluded: they depend on the
# v5 keymap/config system that is not yet wired; youtube.cpp handles
# backend dispatch directly.
set(CORE_SRCS
    src/core/layout.cpp
    src/core/log.cpp
    src/core/paths.cpp
    src/core/proc.cpp
    src/core/queue.cpp
    src/core/share.cpp
    src/core/sponsorblock.cpp
    src/core/text.cpp
    src/core/timer.cpp
    src/core/url.cpp
    src/core/watchstate.cpp
)

set(QRCODEGEN_SRCS
    third_party/qrcodegen/qrcodegen.cpp
)

# ── Common include paths ──────────────────────────────────────────────────────
set(YTCUI_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src          # so "core/timer.h" resolves to src/core/timer.h
    ${CMAKE_SOURCE_DIR}/third_party/ytcui-dl/include
    # ytcui-we headers are intentionally NOT on the main include path.
    # ytcui-we yj.h differs from ytcui-dl yj.h; including both in the same
    # binary causes ODR violations. ytcui-we is a separate executable
    # (third_party/ytcui-we-src) invoked via subprocess from player_we.cpp.
    ${CMAKE_SOURCE_DIR}/third_party/qrcodegen
    ${CURSES_INCLUDE_DIRS}
)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(ytcui
    ${APP_SRCS}
    ${CORE_SRCS}
    ${QRCODEGEN_SRCS}
)

target_include_directories(ytcui PRIVATE ${YTCUI_INCLUDE_DIRS})

target_compile_definitions(ytcui PRIVATE
    YTUI_VERSION="${YTCUI_VERSION_STR}"
    YTCUI_VERSION="${YTCUI_VERSION_STR}"
    USE_YTCUIDL          # default backend: ytcui-dl (native InnerTube, no yt-dlp dep)
    # USE_YTCUIWE is no longer needed: ytcui-we is a subprocess, not a linked lib
)

target_link_libraries(ytcui PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    ${CURSES_LIBRARIES}
    Threads::Threads
)

# ── Compiler flags ────────────────────────────────────────────────────────────
target_compile_options(ytcui PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    >
    # gcc < 13 raises a false -Werror=restrict from inside libstdc++
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,13.0>>:-Wno-restrict>
)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

# Core test include dirs (no ncurses)
set(CORE_TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/ytcui-dl/include
    ${CMAKE_SOURCE_DIR}/third_party/qrcodegen
)

set(CORE_TEST_LIBS OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)

set(CORE_TEST_FLAGS
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,13.0>>:-Wno-restrict>
)

# Helper macro: add a test executable that links only the portable core
macro(add_core_test TEST_SRC)
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC} ${CORE_SRCS} ${QRCODEGEN_SRCS})
    target_include_directories(${TEST_NAME} PRIVATE ${CORE_TEST_INCLUDE_DIRS})
    target_compile_definitions(${TEST_NAME} PRIVATE
        YTUI_VERSION="${YTCUI_VERSION_STR}"
        YTCUI_VERSION="${YTCUI_VERSION_STR}")
    target_link_libraries(${TEST_NAME} PRIVATE ${CORE_TEST_LIBS})
    target_compile_options(${TEST_NAME} PRIVATE ${CORE_TEST_FLAGS})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endmacro()

add_core_test(tests/test_timer.cpp)
add_core_test(tests/test_url.cpp)
add_core_test(tests/test_text.cpp)
add_core_test(tests/test_layout.cpp)
add_core_test(tests/test_sponsorblock.cpp)
add_core_test(tests/test_watchstate.cpp)
add_core_test(tests/test_share.cpp)
add_core_test(tests/test_queue.cpp)

# ── test_player_we: proves ytcui-we backend reaches real resolver ─────────────
add_executable(test_player_we
    tests/test_player_we.cpp
    src/player.cpp
    src/player_resolve.cpp
    src/mpv_ipc.cpp
    src/player_we.cpp
    ${CORE_SRCS}
    ${QRCODEGEN_SRCS}
)
target_include_directories(test_player_we PRIVATE ${CORE_TEST_INCLUDE_DIRS})
target_compile_definitions(test_player_we PRIVATE
    YTUI_VERSION="${YTCUI_VERSION_STR}"
    YTCUI_VERSION="${YTCUI_VERSION_STR}"
    USE_YTCUIDL)
target_link_libraries(test_player_we PRIVATE ${CORE_TEST_LIBS})
target_compile_options(test_player_we PRIVATE ${CORE_TEST_FLAGS})
add_test(NAME test_player_we COMMAND test_player_we $<TARGET_FILE:ytcui-we>)


# ── ytcui-we companion executable ─────────────────────────────────────────────
# Built as a completely separate binary to isolate its yj.h from ytcui-dl's yj.h
# (ODR: both define namespace yj with differing implementations).
# ytcui is a subprocess caller; it never links against ytcui-we headers.
add_subdirectory(third_party/ytcui-we-src)
set_target_properties(ytcui-we PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(ytcui ytcui-we)
add_dependencies(test_player_we ytcui-we)
set_tests_properties(test_player_we PROPERTIES ENVIRONMENT "YTCUI_WE_BINARY=$<TARGET_FILE:ytcui-we>")
add_test(NAME companion_smoke COMMAND $<TARGET_FILE:ytcui-we> --help)
install(TARGETS ytcui RUNTIME DESTINATION bin)

add_executable(test_runtime tests/test_runtime.cpp src/config.cpp src/core/proc.cpp)
target_include_directories(test_runtime PRIVATE ${CORE_TEST_INCLUDE_DIRS})
target_link_libraries(test_runtime PRIVATE ${CORE_TEST_LIBS})
add_test(NAME test_runtime COMMAND test_runtime)


set(APP_TEST_SRCS ${APP_SRCS})
list(REMOVE_ITEM APP_TEST_SRCS src/main.cpp)
add_executable(test_app_lifetime tests/test_app_lifetime.cpp ${APP_TEST_SRCS} ${CORE_SRCS} ${QRCODEGEN_SRCS})
target_include_directories(test_app_lifetime PRIVATE ${YTCUI_INCLUDE_DIRS})
target_compile_definitions(test_app_lifetime PRIVATE YTUI_VERSION="${YTCUI_VERSION_STR}" YTCUI_VERSION="${YTCUI_VERSION_STR}" USE_YTCUIDL)
target_link_libraries(test_app_lifetime PRIVATE ${CORE_TEST_LIBS} ${CURSES_LIBRARIES})
add_test(NAME test_app_lifetime COMMAND test_app_lifetime)
add_test(NAME installed_bundle COMMAND ${CMAKE_COMMAND} "-DBUILD_DIR=${CMAKE_BINARY_DIR}" -P ${CMAKE_SOURCE_DIR}/tests/installed_bundle.cmake)
