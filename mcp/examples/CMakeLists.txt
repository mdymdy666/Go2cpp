add_executable(mcp_agent agent_demo.cpp)
target_link_libraries(mcp_agent PRIVATE mcp_core)
target_compile_options(mcp_agent PRIVATE -Wall -Wextra -Wpedantic)
