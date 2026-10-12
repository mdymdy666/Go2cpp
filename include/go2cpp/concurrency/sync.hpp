#pragma once

// 兼容路径转发头。`sync` 的真身位于 `include/go2cpp/sync.hpp`，本文件此前
// 携带一份过时副本（缺 WaitBackend::ParkForCleanup 与
// Mutex::LockForConditionWait 等成员），已改为转发，避免两处定义漂移。
#include "go2cpp/sync.hpp"
