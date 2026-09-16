option(GO2CPP_BUILD_TESTS "Build Go2Cpp runtime tests" ON)
option(GO2CPP_BUILD_EXAMPLES "Build Go2Cpp runtime examples" ON)
option(GO2CPP_ENABLE_TSAN "Build with ThreadSanitizer when supported" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
