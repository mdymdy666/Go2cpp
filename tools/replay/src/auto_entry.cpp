/**
 * @file auto_entry.cpp
 * @brief `RECORD_FIBER_ENTRY` 注册表与自动重放入口。
 */

#include "replay/auto_entry.hpp"

#include <mutex>
#include <string>
#include <utility>

namespace go2cpp::replay {
namespace {

struct Registry {
    std::mutex mutex;
    FiberEntry entry{nullptr};
    std::string name;
};

Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

}  // namespace

bool RegisterFiberEntry(FiberEntry entry, const char* name) {
    if (entry == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(GetRegistry().mutex);
    if (GetRegistry().entry != nullptr) {
        return false;  // 只保留第一个；多入口场景请显式用 ReplayFileWith
    }
    GetRegistry().entry = entry;
    GetRegistry().name = name == nullptr ? std::string{} : std::string(name);
    return true;
}

bool HasFiberEntry() {
    std::lock_guard<std::mutex> lock(GetRegistry().mutex);
    return GetRegistry().entry != nullptr;
}

FiberEntry GetFiberEntry() {
    std::lock_guard<std::mutex> lock(GetRegistry().mutex);
    return GetRegistry().entry;
}

const char* FiberEntryName() {
    std::lock_guard<std::mutex> lock(GetRegistry().mutex);
    return GetRegistry().name.c_str();
}

ReplayReport ReplayFileAuto(const std::string& path, const ReplayOptions& options) {
    const FiberEntry entry = GetFiberEntry();
    std::string entry_name = FiberEntryName();
    if (entry == nullptr) {
        ReplayReport report;
        report.path = path;
        report.status = ReplayStatus::kNotRun;
        report.error =
            "no fiber entry registered: use RECORD_FIBER_ENTRY(fn) in the "
            "program, or call ReplayFileWith(path, factory) explicitly";
        return report;
    }
    auto factory = [entry](std::uint32_t gid, ReplaySource& source) {
        return [entry, gid, &source]() { entry(gid, source); };
    };
    ReplayReport report = ReplayFileWith(path, factory, options);
    report.auto_entry = true;
    report.entry_name = std::move(entry_name);
    return report;
}

}  // namespace go2cpp::replay
