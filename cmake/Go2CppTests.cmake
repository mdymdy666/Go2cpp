if (GO2CPP_BUILD_TESTS)
    enable_testing()

    add_executable(go2cpp_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_error.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_context.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dynamic_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_panic_defer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fiber.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sync.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_io.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_timer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_beginner_api.cpp
    )
    target_link_libraries(go2cpp_tests PRIVATE go2cpp_runtime)
    if (TARGET go2cpp_hook)
        target_sources(go2cpp_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hook.cpp)
        target_compile_definitions(go2cpp_tests PRIVATE GO2CPP_TEST_HOOK=1)
    endif()
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
    set(GO2CPP_EXAMPLES runtime_demo fiber_sync_demo managed_pipeline_demo
                        dynamic_gmp_demo mixed_runtime_demo beginner_demo)
    if (TARGET go2cpp_hook)
        list(APPEND GO2CPP_EXAMPLES io_hook_demo)
    endif()
    foreach(example_name IN LISTS GO2CPP_EXAMPLES)
        add_executable(go2cpp_${example_name}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/${example_name}.cpp)
        target_link_libraries(go2cpp_${example_name} PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_${example_name} PRIVATE
            -Wall -Wextra -Wpedantic)
        if (GO2CPP_BUILD_TESTS)
            add_test(NAME go2cpp_${example_name} COMMAND go2cpp_${example_name})
            set_tests_properties(go2cpp_${example_name} PROPERTIES TIMEOUT 30)
        endif()
    endforeach()
endif()
