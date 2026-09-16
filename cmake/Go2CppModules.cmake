include(GNUInstallDirs)
find_package(Threads REQUIRED)

add_library(go2cpp_error STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/error.cpp)
add_library(go2cpp_context STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/context.cpp)
add_library(go2cpp_channel STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/channel.cpp)
add_library(go2cpp_scheduler STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler.cpp)
add_library(go2cpp_panic_defer STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panic_defer.cpp)

# The public umbrella target keeps one stable link name while the module
# targets remain independently inspectable and replaceable by embedders.
add_library(go2cpp_runtime INTERFACE)
add_library(go2cpp::runtime ALIAS go2cpp_runtime)

set_target_properties(go2cpp_error PROPERTIES EXPORT_NAME error)
set_target_properties(go2cpp_context PROPERTIES EXPORT_NAME context)
set_target_properties(go2cpp_channel PROPERTIES EXPORT_NAME channel)
set_target_properties(go2cpp_scheduler PROPERTIES EXPORT_NAME scheduler)
set_target_properties(go2cpp_panic_defer PROPERTIES EXPORT_NAME panic_defer)
set_target_properties(go2cpp_runtime PROPERTIES EXPORT_NAME runtime)

# Keep the source-tree and installed package spellings identical. The aliases
# are not a second library or ABI; they only make add_subdirectory consumers
# use the same names as find_package consumers.
add_library(go2cpp::error ALIAS go2cpp_error)
add_library(go2cpp::context ALIAS go2cpp_context)
add_library(go2cpp::channel ALIAS go2cpp_channel)
add_library(go2cpp::scheduler ALIAS go2cpp_scheduler)
add_library(go2cpp::panic_defer ALIAS go2cpp_panic_defer)

set(GO2CPP_MODULE_TARGETS
    go2cpp_error
    go2cpp_context
    go2cpp_channel
    go2cpp_scheduler
    go2cpp_panic_defer
)

foreach(module_target IN LISTS GO2CPP_MODULE_TARGETS)
    target_include_directories(${module_target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_features(${module_target} PUBLIC cxx_std_17)
    target_compile_options(${module_target} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(${module_target} PUBLIC Threads::Threads)
endforeach()

target_include_directories(go2cpp_runtime INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(go2cpp_context PUBLIC go2cpp_error)
target_link_libraries(go2cpp_channel PUBLIC go2cpp_context go2cpp_error
                                      go2cpp_panic_defer)
target_link_libraries(go2cpp_scheduler PUBLIC go2cpp_panic_defer)
target_link_libraries(go2cpp_runtime INTERFACE
    go2cpp_error go2cpp_context go2cpp_channel go2cpp_scheduler
    go2cpp_panic_defer Threads::Threads)

if (GO2CPP_ENABLE_TSAN)
    foreach(module_target IN LISTS GO2CPP_MODULE_TARGETS)
        target_compile_options(${module_target} PUBLIC
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${module_target} PUBLIC -fsanitize=thread)
    endforeach()
endif()
