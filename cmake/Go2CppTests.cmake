if (GO2CPP_BUILD_TESTS)
    enable_testing()

    add_executable(go2cpp_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_error.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_context.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_panic_defer.cpp
    )
    target_link_libraries(go2cpp_tests PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_tests COMMAND go2cpp_tests)
    set_tests_properties(go2cpp_tests PROPERTIES TIMEOUT 30)

    add_executable(go2cpp_scheduler_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/scheduler_smoke.cpp)
    target_compile_definitions(go2cpp_scheduler_smoke PRIVATE
        GO2CPP_SCHEDULER_SMOKE_MAIN=1)
    target_link_libraries(go2cpp_scheduler_smoke PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_scheduler_smoke PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_scheduler_smoke COMMAND go2cpp_scheduler_smoke)
    set_tests_properties(go2cpp_scheduler_smoke PROPERTIES TIMEOUT 30)
endif()

if (GO2CPP_BUILD_EXAMPLES)
    add_executable(go2cpp_runtime_demo
        ${CMAKE_CURRENT_SOURCE_DIR}/example/runtime_demo.cpp)
    target_link_libraries(go2cpp_runtime_demo PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_runtime_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()
