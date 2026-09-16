include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ${GO2CPP_MODULE_TARGETS} go2cpp_runtime
    EXPORT go2cpp_runtime_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT go2cpp_runtime_targets
    FILE go2cpp_runtime-targets.cmake
    NAMESPACE go2cpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/go2cpp_runtime
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/go2cpp_runtimeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/go2cpp_runtimeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/go2cpp_runtime
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/go2cpp_runtimeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/go2cpp_runtimeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/go2cpp_runtimeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/go2cpp_runtime
)
