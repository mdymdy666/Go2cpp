#pragma once

// 模块入口转发头：Go `io` 包（流接口族）的对应物。
// 真身位于 `go2cpp/tools/streams.hpp`；命名空间为 `go2cpp::streams`，
// 因为 `go2cpp::io` 已被「描述符与多路复用调度层」占用（见 network/io.hpp）。
#include "go2cpp/tools/streams.hpp"
