#pragma once

/**
 * @file rpc.hpp
 * @brief RPC 模块聚合头：状态、注册表、协议、传输、服务端、客户端。
 *
 * @details 包含本文件即可使用除 WebSocket 适配器外的全部能力；
 *          WebSocket 适配器需要 `go2cpp/network/websocket.hpp`，因此单独放在
 *          `go2cpp/rpc/websocket_transport.hpp`，不进入默认聚合头（避免让
 *          RPC 核心库间接依赖 network 模块）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/rpc/client.hpp"
#include "go2cpp/rpc/protocol.hpp"
#include "go2cpp/rpc/registry.hpp"
#include "go2cpp/rpc/server.hpp"
#include "go2cpp/rpc/status.hpp"
#include "go2cpp/rpc/transport.hpp"
