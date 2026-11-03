cmake_minimum_required(VERSION 3.16)

# Read version from file
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" YTCUI_VERSION_STR LIMIT_COUNT 1)
string(STRIP "${YTCUI_VERSION_STR}" YTCUI_VERSION_STR)

project(ytcui VERSION ${YTCUI_VERSION_STR} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Find dependencies ─────────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

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
)

target_link_libraries(ytcui PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    ${CURSES_LIBRARIES}
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

set(CORE_TEST_LIBS OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

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
