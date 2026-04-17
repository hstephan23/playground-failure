cmake_minimum_required(VERSION 3.20)
project(playground LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)        # need POSIX (kqueue, sigaction, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- sanitizer build types ----
set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
set(CMAKE_C_FLAGS_ASAN  "-O1 -g -fsanitize=address  -fno-omit-frame-pointer -fno-optimize-sibling-calls")
set(CMAKE_C_FLAGS_TSAN  "-O1 -g -fsanitize=thread   -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_UBSAN "-O1 -g -fsanitize=undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_ASAN  "-fsanitize=address")
set(CMAKE_EXE_LINKER_FLAGS_TSAN  "-fsanitize=thread")
set(CMAKE_EXE_LINKER_FLAGS_UBSAN "-fsanitize=undefined")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -fno-omit-frame-pointer
)

include_directories(include src)

# ---- dependencies ----
set(CURSES_NEED_NCURSES TRUE)
find_package(Curses REQUIRED)
find_package(Threads REQUIRED)

# panel lib lives separately on macOS Homebrew; FindCurses doesn't always pick it up
find_library(PANEL_LIB NAMES panel HINTS ${CURSES_LIBRARY_DIRS})

# ---- source globs (auto-discover scenarios) ----
file(GLOB CORE_SRC      CONFIGURE_DEPENDS src/core/*.c)
file(GLOB TUI_SRC       CONFIGURE_DEPENDS src/tui/*.c)
file(GLOB CHAOS_SRC     CONFIGURE_DEPENDS src/chaos/*.c)
file(GLOB_RECURSE SCENARIO_SRC CONFIGURE_DEPENDS src/scenarios/*.c)

# ---- main interactive binary ----
add_executable(playground
    src/main.c
    ${CORE_SRC}
    ${TUI_SRC}
    ${CHAOS_SRC}
    ${SCENARIO_SRC}
)
target_include_directories(playground PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(playground PRIVATE ${CURSES_LIBRARIES} Threads::Threads)
if(PANEL_LIB)
    target_link_libraries(playground PRIVATE ${PANEL_LIB})
endif()
target_compile_definitions(playground PRIVATE PG_HAS_TUI=1)

# ---- headless runner (no ncurses, no TUI sources) ----
add_executable(playground_run
    src/main.c
    ${CORE_SRC}
    ${CHAOS_SRC}
    ${SCENARIO_SRC}
)
target_link_libraries(playground_run PRIVATE Threads::Threads)
