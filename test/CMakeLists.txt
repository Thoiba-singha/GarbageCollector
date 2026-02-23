
# ── Test executable ───────────────────────────────────────────────────────────
add_executable(unit_tests
    cpp_test.cpp
)

target_link_libraries(unit_tests
    PRIVATE
        collections
        project_warnings
        project_sanitizers
)

set_target_properties(unit_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

