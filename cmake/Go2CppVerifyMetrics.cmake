# =============================================================================
# cmake/Go2CppVerifyMetrics.cmake — verify_metrics 的 CMake 钩子模板（opt-in）
#
# 重要：本文件**不被任何 CMakeLists.txt include**，默认不参与构建，也不加入
#       ALL；它只是给需要的人手动接线的模板。仓库构建行为不因它而改变。
#
# 用法 A（不改 CMakeLists.txt，script 模式直接跑）：
#   cmake -P cmake/Go2CppVerifyMetrics.cmake
#   cmake -DGO2CPP_VERIFY_ARGS=--quick -P cmake/Go2CppVerifyMetrics.cmake
#   （GO2CPP_VERIFY_ARGS 原样传给 tools/verify_metrics.sh，可填 --quick/--json/--refs）
#
# 用法 B（手动 opt-in 成构建目标；需要自己在顶层 CMakeLists.txt 加两行）：
#   option(GO2CPP_VERIFY_METRICS "启用 verify-metrics 目标" OFF)
#   if(GO2CPP_VERIFY_METRICS)
#     include(cmake/Go2CppVerifyMetrics.cmake)
#   endif()
#   然后：
#     cmake -DGO2CPP_VERIFY_METRICS=ON ... && cmake --build . --target verify-metrics
#
# 退出码与脚本一致：0 = 无 STALE（OK+SKIP）；1 = 有 STALE；2 = 环境错误。
# =============================================================================

if(CMAKE_SCRIPT_MODE_FILE)
  # ---- 用法 A：cmake -P 直接执行，不定义任何 target ------------------------
  get_filename_component(_go2cpp_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  execute_process(
    COMMAND bash "${_go2cpp_root}/tools/verify_metrics.sh" ${GO2CPP_VERIFY_ARGS}
    WORKING_DIRECTORY "${_go2cpp_root}"
    RESULT_VARIABLE _go2cpp_verify_rc)
  message(STATUS "verify_metrics 退出码: ${_go2cpp_verify_rc}（0=无 STALE，1=有 STALE，2=环境错误）")
  if(NOT _go2cpp_verify_rc EQUAL 0)
    message(FATAL_ERROR "verify_metrics 未通过（rc=${_go2cpp_verify_rc}）")
  endif()
else()
  # ---- 用法 B：被显式 include 时定义一个 opt-in 目标，不加入 ALL ----------
  add_custom_target(verify-metrics
      COMMAND bash ${CMAKE_SOURCE_DIR}/tools/verify_metrics.sh
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "校验 docs/metrics-ledger.md 的实测值（opt-in，不默认加入 ALL）"
      VERBATIM)
endif()
