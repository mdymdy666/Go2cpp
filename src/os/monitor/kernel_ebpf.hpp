#pragma once

// 三个 tracepoint 监控器共用的 eBPF 加载/挂载/读取工具。
//
// 为什么抽出来：三个监控器的 ELF 加载、root 检测、tracepoint attach、map
// 遍历逻辑完全一致；各写一份会让「非 root 降级」出现三种不一致的 reason。
// 只在 src 内部使用，不安装到 include/，libbpf 头文件也不会泄漏给调用方。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(GO2CPP_HAVE_EBPF)
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#endif

namespace go2cpp::os::monitor::detail {

/** @brief 当前进程能否加载 eBPF（root + 构建期启用了 libbpf/clang）。 */
bool KernelBpfUsable();

/** @brief 不可用原因；可用时返回空串。 */
std::string KernelBpfUnusableReason();

/** @brief 一个已加载的 BPF 对象；析构时自动 detach + close。 */
class KernelBpfObject {
public:
    KernelBpfObject();
    ~KernelBpfObject();
    KernelBpfObject(const KernelBpfObject&) = delete;
    KernelBpfObject& operator=(const KernelBpfObject&) = delete;

    /** @brief 从内嵌 ELF 字节加载对象；失败时写 *error。 */
    bool Load(const unsigned char* elf, std::size_t len, std::string* error);

    /** @brief 按名字查 map fd；不存在返回 -1。 */
    int MapFd(const char* name) const;

    /** @brief attach 一个 tracepoint 程序；失败时写 *error。 */
    bool AttachTracepoint(const char* program, const char* category,
                          const char* name, std::string* error);

    /**
     * @brief attach 一个 kprobe/kretprobe 程序。
     * @param program BPF 程序名（`SEC("kprobe/...")` 对应的函数名）
     * @param function 内核函数符号，如 "vfs_read"
     * @param retprobe true 表示 kretprobe（函数返回时触发）
     * @param error 失败原因
     */
    bool AttachKprobe(const char* program, const char* function, bool retprobe,
                      std::string* error);

    /**
     * @brief 分离最近 attach 的 count 个 link。
     *
     * 可选探针按「entry+return」成对 attach；return 侧失败时回滚刚 attach
     * 的 entry 侧，避免留下永不结算的起始时间记录。count 大于已 attach
     * 数量时只分离现有的。
     */
    void DetachLast(std::size_t count);

    /** @brief 分离所有 link 并关闭对象；可重复调用。 */
    void Close();

    bool loaded() const noexcept;

    /**
     * @brief 已 attach 程序的累计内核执行时间与次数。
     *
     * 数据来自 `bpf_prog_info.run_time_ns/run_cnt`，由内核 BPF JIT 记账，
     * 是「eBPF 自身 CPU 开销」最直接的证据。内核未开启统计时两者为 0。
     */
    void ProgramCost(std::uint64_t* run_time_ns, std::uint64_t* run_cnt) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief 遍历 HASH map 的全部键值。
 *
 * 用 `bpf_map_get_next_key` 的 NULL 起点取第一项，再迭代后继，避免漏掉
 * key==0 的条目。读取中途失败时返回已经读到的部分（监控场景下宁可少一条
 * 样本，也不要因为一个条目消失就整次采样失败）。
 */
template <typename Key, typename Value>
std::vector<std::pair<Key, Value>> ReadHashMap(int fd) {
    std::vector<std::pair<Key, Value>> result;
#if defined(GO2CPP_HAVE_EBPF)
    if (fd < 0) {
        return result;
    }
    Key key{};
    Key next{};
    bool first = true;
    for (;;) {
        if (bpf_map_get_next_key(fd, first ? nullptr : &key, &next) != 0) {
            break;
        }
        first = false;
        Value value{};
        if (bpf_map_lookup_elem(fd, &next, &value) == 0) {
            result.emplace_back(next, value);
        }
        key = next;
    }
#else
    (void)fd;
#endif
    return result;
}

/** @brief 读取 ARRAY map 的单条记录；失败返回 false。 */
template <typename Value>
bool ReadArrayMap(int fd, std::uint32_t index, Value* out) {
#if defined(GO2CPP_HAVE_EBPF)
    if (fd < 0 || out == nullptr) {
        return false;
    }
    return bpf_map_lookup_elem(fd, &index, out) == 0;
#else
    (void)fd;
    (void)index;
    (void)out;
    return false;
#endif
}

}  // namespace go2cpp::os::monitor::detail
