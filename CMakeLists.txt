cmake_minimum_required(VERSION 3.25)

project(
    Collections
    VERSION 1.0.0
    DESCRIPTION "Production-grade C/C++ collections library"
    LANGUAGES C CXX
)

# ── Global C++ standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build type default ─────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# ── Shared interface targets ───────────────────────────────────────────────────
# Empty interface target — kept so all target_link_libraries references stay valid.
add_library(project_warnings INTERFACE)

# Sanitizers (opt-in, useful in CI / Debug)
option(ENABLE_SANITIZERS "Enable ASan + UBSan in Debug builds" OFF)
add_library(project_sanitizers INTERFACE)
if(ENABLE_SANITIZERS)
    target_compile_options(project_sanitizers INTERFACE
        $<$<AND:$<CXX_COMPILER_ID:GNU,Clang,AppleClang>,$<CONFIG:Debug>>:
            -fsanitize=address,undefined -fno-omit-frame-pointer
        >
    )
    target_link_options(project_sanitizers INTERFACE
        $<$<AND:$<CXX_COMPILER_ID:GNU,Clang,AppleClang>,$<CONFIG:Debug>>:
            -fsanitize=address,undefined
        >
    )
endif()

# ── Export compile commands (clangd / IDE support) ────────────────────────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Sub-projects ───────────────────────────────────────────────────────────────
add_subdirectory(collections)

option(BUILD_TESTS "Build the test suite" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()