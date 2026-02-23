

# ── C sub-library (Allocator) ─────────────────────────────────────────────────
add_library(c_collections OBJECT
    c/Allocator.cpp
)

target_include_directories(c_collections
    PUBLIC
        # One level up so consumers write: #include "collections/meta.h"
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(c_collections
    PRIVATE
        project_warnings
        project_sanitizers
)

# Pure C compilation unit: enforce the C++ standard only where needed.
set_target_properties(c_collections PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# ── C++ sub-library (gc, VSharedPtr) ─────────────────────────────────────────
add_library(cpp_collections OBJECT
    cpp/gc.cpp
)

target_include_directories(cpp_collections
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(cpp_collections
    PRIVATE
        project_warnings
        project_sanitizers
)

set_target_properties(cpp_collections PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# ── Merged static library ─────────────────────────────────────────────────────
add_library(collections STATIC)

target_link_libraries(collections
    PUBLIC
        c_collections
        cpp_collections
)

# Expose project_root as the include root so consumers write:
#   #include "collections/meta.h"
target_include_directories(collections
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(collections PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# ── Install rules ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS collections
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    meta.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/collections
)

install(FILES
    cpp/gc.hpp
    cpp/VSharedPtr.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/collections/cpp
)