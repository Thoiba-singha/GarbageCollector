# CMakeList.txt : CMake project for GC, include source and define

cmake_minimum_required (VERSION 3.15)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("GC" C CXX)

set(CMAKE_CXX_STANDARD 20)

file(GLOB C_FILES "gc/c/Allocator.cpp")

file(GLOB CPP_FILES "gc/cpp/Ptr.hpp" "gc/cpp/VSharedPtr.hpp")

# Executable for C
# add_executable (${PROJECT_NAME} ${C_FILES} "c_test.c")

# Executable for C++
add_executable (${PROJECT_NAME} ${CPP_FILES} "cpp_test.cpp")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET GC PROPERTY CXX_STANDARD 20)
endif()

