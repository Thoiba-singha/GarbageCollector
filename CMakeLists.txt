# CMakeList.txt : CMake project for collections, include source and define

cmake_minimum_required (VERSION 3.20)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("collections" C CXX)

# Enforce modern C++
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===============================
# Library target
# ===============================
add_library(collections INTERFACE)

# Public include path
target_include_directories(collections
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Enforce modern C++
target_compile_features(collections
    INTERFACE
        cxx_std_23
)


# Warnings (consumer-safe)
if (MSVC)
    target_compile_options(collections INTERFACE /permissive- /W4)
else()
    target_compile_options(collections INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ===============================
# Demo / tests
# ===============================
add_subdirectory(test)

