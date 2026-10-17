/**
 * @file sync.hpp
 * @brief 同步原语模块的兼容转发头。
 *
 * ## 为什么存在
 *
 * `sync` 的真身位于 `include/go2cpp/concurrency/sync.hpp`（同名的另一份）。
 * 保留这条转发路径是为了兼容旧 include 写法，避免调用方因为目录调整而
 * 全部编译失败。
 *
 * ## 注意
 *
 * 这是一个**纯转发头**，不含任何实现。如果在这里补充声明，会与真身产生
 * 两份不一致的副本 —— 新增接口请改真身，不要改这里。
 */
#pragma once

// 兼容路径转发头。`sync` 的真身位于 `include/go2cpp/sync.hpp`，本文件此前
// 携带一份过时副本（缺 WaitBackend::ParkForCleanup 与
// Mutex::LockForConditionWait 等成员），已改为转发，避免两处定义漂移。
#include "go2cpp/sync.hpp"
