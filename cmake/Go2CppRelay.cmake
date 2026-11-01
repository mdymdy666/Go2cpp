# ---------------------------------------------------------------------------
# Relay 汇总入口（稳定文件名，只做 include 调度）。
#
# 本仓库同一工作树里有多个 agent 并行开发，relay 目录由两个需求共同占用：
# - REQ-002 RelayPipe：cmake/Go2CppRelayPipe.cmake（顶层自行 include）
# - REQ-003 Hub / REQ-005 Lease+Heartbeat：cmake/Go2CppRelayHubLease.cmake
#
# 这里**不再** include RelayPipe：它已由顶层 CMakeLists 单独接入，且使用独立
# 目标 `go2cpp_relay_pipe`；重复 include 会导致目标/测试名重复。Hub/Lease 的
# 目标 `go2cpp_relay` 与它互不覆盖：如果将来 RelayPipe 想并入同一个库，只需
# 把 relay_pipe.cpp 追加到 go2cpp_relay 并删除自己的独立目标。
# ---------------------------------------------------------------------------
include(cmake/Go2CppRelayHubLease.cmake)
