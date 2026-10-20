#pragma once

/**
 * @file doctor.hpp
 * @brief `go2cpp-cli doctor`：通过 G2CD/1 帧协议与目标进程的 Doctor 通信。
 *
 * @details 本子命令**不链接运行时库**：帧编解码来自
 *          `go2cpp/debug/diagnostic_protocol.hpp`（header-only），
 *          传输是本地 Unix socket；QUIC 传输见 `--transport` 的后续落地说明。
 */

#include <string>
#include <vector>

namespace go2cpp::cli {

/// 执行 `doctor` 子命令；返回进程退出码（不抛异常，错误统一转成退出码）。
int RunDoctor(const std::vector<std::string>& args);

}  // namespace go2cpp::cli
