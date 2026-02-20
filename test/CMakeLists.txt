# CMakeList.txt : CMake project for GC_Collectors, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (demo "cpp_test.cpp")


target_link_libraries(demo
    PRIVATE
        collections
)



# TODO: Add tests and install targets if needed.
