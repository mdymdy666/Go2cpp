include(GNUInstallDirs)
find_package(Threads REQUIRED)
find_package(Boost 1.70 REQUIRED COMPONENTS context)
include(CheckIncludeFileCXX)
check_include_file_cxx(valgrind/valgrind.h GO2CPP_HAVE_VALGRIND_HEADER)

option(BUILD_SHARED_LIBS "Build shared runtime modules" ON)
option(GO2CPP_BUILD_HOOK "Build the Linux syscall interposer" ON)
if (GO2CPP_BUILD_HOOK AND NOT BUILD_SHARED_LIBS)
    message(FATAL_ERROR "The hook requires shared runtime modules to keep one TLS/FD registry; use GO2CPP_BUILD_HOOK=OFF for a static runtime")
endif()

add_library(go2cpp_error ${CMAKE_CURRENT_SOURCE_DIR}/src/error.cpp)
add_library(go2cpp_context ${CMAKE_CURRENT_SOURCE_DIR}/src/context.cpp)
add_library(go2cpp_channel ${CMAKE_CURRENT_SOURCE_DIR}/src/channel.cpp)
add_library(go2cpp_scheduler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parking_condition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thread_policy.cpp)
add_library(go2cpp_fiber
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fiber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fiber_local.cpp)
add_library(go2cpp_sync ${CMAKE_CURRENT_SOURCE_DIR}/src/sync.cpp)
add_library(go2cpp_future ${CMAKE_CURRENT_SOURCE_DIR}/src/future.cpp)
add_library(go2cpp_io ${CMAKE_CURRENT_SOURCE_DIR}/src/io.cpp)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND GO2CPP_BUILD_HOOK)
    add_library(go2cpp_hook SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/hook.cpp)
endif()

# The public umbrella target keeps one stable link name while the module
# targets remain independently inspectable and replaceable by embedders.
add_library(go2cpp_runtime INTERFACE)
add_library(go2cpp::runtime ALIAS go2cpp_runtime)

set_target_properties(go2cpp_error PROPERTIES EXPORT_NAME error)
set_target_properties(go2cpp_context PROPERTIES EXPORT_NAME context)
set_target_properties(go2cpp_channel PROPERTIES EXPORT_NAME channel)
set_target_properties(go2cpp_scheduler PROPERTIES EXPORT_NAME scheduler)
set_target_properties(go2cpp_fiber PROPERTIES EXPORT_NAME fiber)
set_target_properties(go2cpp_sync PROPERTIES EXPORT_NAME sync)
set_target_properties(go2cpp_future PROPERTIES EXPORT_NAME future)
set_target_properties(go2cpp_io PROPERTIES EXPORT_NAME io)
if (TARGET go2cpp_hook)
    set_target_properties(go2cpp_hook PROPERTIES EXPORT_NAME hook)
endif()
set_target_properties(go2cpp_runtime PROPERTIES EXPORT_NAME runtime)

# Keep the source-tree and installed package spellings identical. The aliases
# are not a second library or ABI; they only make add_subdirectory consumers
# use the same names as find_package consumers.
add_library(go2cpp::error ALIAS go2cpp_error)
add_library(go2cpp::context ALIAS go2cpp_context)
add_library(go2cpp::channel ALIAS go2cpp_channel)
add_library(go2cpp::scheduler ALIAS go2cpp_scheduler)
add_library(go2cpp::fiber ALIAS go2cpp_fiber)
add_library(go2cpp::sync ALIAS go2cpp_sync)
add_library(go2cpp::future ALIAS go2cpp_future)
add_library(go2cpp::io ALIAS go2cpp_io)
if (TARGET go2cpp_hook)
    add_library(go2cpp::hook ALIAS go2cpp_hook)
endif()

set(GO2CPP_MODULE_TARGETS
    go2cpp_error
    go2cpp_context
    go2cpp_channel
    go2cpp_scheduler
    go2cpp_fiber
    go2cpp_sync
    go2cpp_future
    go2cpp_io
)
if (TARGET go2cpp_hook)
    list(APPEND GO2CPP_MODULE_TARGETS go2cpp_hook)
endif()

foreach(module_target IN LISTS GO2CPP_MODULE_TARGETS)
    target_include_directories(${module_target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_features(${module_target} PUBLIC cxx_std_17)
    set_target_properties(${module_target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION 0)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(${module_target} PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    target_compile_options(${module_target} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(${module_target} PUBLIC Threads::Threads)
endforeach()

target_include_directories(go2cpp_runtime INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(go2cpp_context PUBLIC go2cpp_error go2cpp_scheduler)
target_link_libraries(go2cpp_channel PUBLIC go2cpp_context go2cpp_error)
target_link_libraries(go2cpp_scheduler PUBLIC go2cpp_fiber)
target_link_libraries(go2cpp_fiber PUBLIC Boost::context)
if (GO2CPP_HAVE_VALGRIND_HEADER)
    target_compile_definitions(go2cpp_fiber PRIVATE BOOST_USE_VALGRIND=1)
endif()
target_link_libraries(go2cpp_sync PUBLIC go2cpp_context go2cpp_scheduler)
target_link_libraries(go2cpp_future PUBLIC go2cpp_context go2cpp_scheduler)
target_link_libraries(go2cpp_io PUBLIC go2cpp_context go2cpp_scheduler)
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_hook PUBLIC go2cpp_io go2cpp_fiber
                          ${CMAKE_DL_LIBS})
endif()
target_link_libraries(go2cpp_runtime INTERFACE
    go2cpp_error go2cpp_context go2cpp_channel go2cpp_scheduler
    go2cpp_fiber go2cpp_sync go2cpp_future go2cpp_io
    Threads::Threads)
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_runtime INTERFACE go2cpp_hook)
    # An interposer provides symbols through libc calls, not explicit API
    # references. Keep the DSO even when the executable never names a hook.
    target_link_options(go2cpp_runtime INTERFACE "LINKER:--no-as-needed")
endif()

if (GO2CPP_ENABLE_TSAN)
    foreach(module_target IN LISTS GO2CPP_MODULE_TARGETS)
        target_compile_options(${module_target} PUBLIC
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${module_target} PUBLIC -fsanitize=thread)
    endforeach()
endif()
