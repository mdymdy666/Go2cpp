/**
 * @file safepoints.cpp
 * @brief 挂起点注册表的实现：扫描已加载模块的 `migration_safepoints` 段。
 *
 * 恢复进程在**不执行用户代码**的前提下拿到 `id → 锚点地址` 映射，是
 * safepoint 方案能替代指令偏移的关键。这里用 `dl_iterate_phdr` 枚举模块，
 * 再读磁盘上的 ELF 段表定位自定义段，最后用「加载基址 + sh_addr」算出
 * 运行期地址 —— 主程序（/proc/self/exe）与 dlopen 的 SO 走同一条路径。
 */
#include "migration/safepoints.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace migration {
namespace {

struct ModuleInfo {
    std::string path;
    std::uint64_t bias{0};
};

int CollectModule(struct dl_phdr_info* info, std::size_t /*size*/, void* data) {
    auto* modules = static_cast<std::vector<ModuleInfo>*>(data);
    const char* name = info->dlpi_name;
    ModuleInfo module;
    if (name == nullptr || name[0] == '\0') {
        module.path = "/proc/self/exe";
    } else {
        module.path = name;
    }
    module.bias = static_cast<std::uint64_t>(info->dlpi_addr);
    // 同一个文件可能以不同路径出现（/proc/self/exe 与 SO 不会重复）；
    // 这里只按 (path, bias) 去重，避免重复计入同一段。
    for (const auto& existing : *modules) {
        if (existing.path == module.path && existing.bias == module.bias) return 0;
    }
    modules->push_back(std::move(module));
    return 0;
}

std::vector<ModuleInfo> LoadedModules() {
    std::vector<ModuleInfo> modules;
    ::dl_iterate_phdr(CollectModule, &modules);
    return modules;
}

/// 读取整个 ELF 文件（大小上限 256 MiB，防御异常输入）。
bool ReadElfFile(const std::string& path, std::vector<std::uint8_t>* out,
                 std::string* error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *error = std::string("打开失败: ") + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0 ||
        static_cast<std::uint64_t>(st.st_size) > (256ULL << 20)) {
        ::close(fd);
        *error = "文件长度非法";
        return false;
    }
    out->resize(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < out->size()) {
        const ssize_t n = ::read(fd, out->data() + got, out->size() - got);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (got != out->size()) {
        *error = "读取不完整";
        return false;
    }
    return true;
}

/// 从 ELF 文件里找出名为 `name` 的段，返回 {sh_addr, sh_size}。
bool FindSection(const std::vector<std::uint8_t>& elf, const char* name,
                 std::uint64_t* address, std::uint64_t* size,
                 std::string* error) {
    if (elf.size() < sizeof(Elf64_Ehdr)) {
        *error = "不是合法 ELF（过短）";
        return false;
    }
    Elf64_Ehdr eh {};
    std::memcpy(&eh, elf.data(), sizeof(eh));
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB) {
        *error = "不是 x86-64 小端 ELF";
        return false;
    }
    if (eh.e_shoff == 0 || eh.e_shnum == 0 ||
        eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shstrndx >= eh.e_shnum) {
        *error = "缺少段表";
        return false;
    }
    const std::uint64_t sh_end =
        eh.e_shoff + static_cast<std::uint64_t>(eh.e_shnum) * sizeof(Elf64_Shdr);
    if (sh_end > elf.size()) {
        *error = "段表越过文件末尾";
        return false;
    }
    Elf64_Shdr str_sh {};
    std::memcpy(&str_sh,
                elf.data() + eh.e_shoff +
                    static_cast<std::uint64_t>(eh.e_shstrndx) * sizeof(Elf64_Shdr),
                sizeof(str_sh));
    if (str_sh.sh_offset + str_sh.sh_size > elf.size()) {
        *error = "段名字符串表越界";
        return false;
    }
    const char* strings =
        reinterpret_cast<const char*>(elf.data() + str_sh.sh_offset);
    for (std::uint16_t i = 0; i < eh.e_shnum; ++i) {
        Elf64_Shdr sh {};
        std::memcpy(&sh,
                    elf.data() + eh.e_shoff +
                        static_cast<std::uint64_t>(i) * sizeof(Elf64_Shdr),
                    sizeof(sh));
        if (sh.sh_name >= str_sh.sh_size) continue;
        if (std::strcmp(strings + sh.sh_name, name) != 0) continue;
        *address = sh.sh_addr;
        *size = sh.sh_size;
        return true;
    }
    *error = "未找到段";
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// SafepointTable
// ---------------------------------------------------------------------------
SafepointTable& SafepointTable::Instance() {
    static SafepointTable table;
    return table;
}

void SafepointTable::Refresh() {
    m_entries.clear();
    m_errors.clear();
    m_loaded = true;
    for (const auto& module : LoadedModules()) {
        std::vector<std::uint8_t> elf;
        std::string error;
        if (!ReadElfFile(module.path, &elf, &error)) {
            // vdso 之类没有磁盘文件，忽略；其它失败留给 verify 诊断。
            if (module.path.find("vdso") == std::string::npos) {
                m_errors.push_back(module.path + ": " + error);
            }
            continue;
        }
        std::uint64_t address = 0;
        std::uint64_t size = 0;
        if (!FindSection(elf, "migration_safepoints", &address, &size, &error)) {
            continue;  // 没有挂起点是正常情况（例如 libc）
        }
        if (size % sizeof(SafepointAnchor) != 0) {
            m_errors.push_back(module.path + ": 段长度不是 40 的整数倍");
            continue;
        }
        const auto* anchors = reinterpret_cast<const SafepointAnchor*>(
            static_cast<std::uintptr_t>(module.bias + address));
        const std::size_t count = static_cast<std::size_t>(size / sizeof(SafepointAnchor));
        for (std::size_t i = 0; i < count; ++i) {
            const SafepointAnchor& anchor = anchors[i];
            if (anchor.id == 0 || anchor.address == nullptr) {
                m_errors.push_back(module.path + ": 段内存在 id=0 或空地址的锚点");
                continue;
            }
            SafepointInfo info;
            info.id = anchor.id;
            info.address = reinterpret_cast<std::uint64_t>(anchor.address);
            info.function = anchor.pretty_function != nullptr ? anchor.pretty_function : "";
            info.file = anchor.file != nullptr ? anchor.file : "";
            info.line = anchor.line;
            info.module = module.path;
            m_entries.push_back(std::move(info));
        }
    }
    std::sort(m_entries.begin(), m_entries.end(),
              [](const SafepointInfo& a, const SafepointInfo& b) {
                  if (a.id != b.id) return a.id < b.id;
                  return a.address < b.address;
              });
}

void SafepointTable::EnsureLoaded() const {
    if (!m_loaded) const_cast<SafepointTable*>(this)->Refresh();
}

const SafepointInfo* SafepointTable::Find(std::uint32_t id) const {
    EnsureLoaded();
    for (const auto& entry : m_entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

const SafepointInfo* SafepointTable::FindByAddress(std::uint64_t address) const {
    EnsureLoaded();
    for (const auto& entry : m_entries) {
        if (entry.address == address) return &entry;
    }
    return nullptr;
}

std::vector<SafepointInfo> SafepointTable::Entries() const {
    EnsureLoaded();
    return m_entries;
}

std::vector<std::string> SafepointTable::DuplicateIds() const {
    EnsureLoaded();
    std::vector<std::string> duplicates;
    for (std::size_t i = 1; i < m_entries.size(); ++i) {
        if (m_entries[i].id != m_entries[i - 1].id) continue;
        // 同一 id 的每一段连续区间只报一次。
        if (i >= 2 && m_entries[i - 2].id == m_entries[i].id) continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "挂起点 id=0x%x 出现在多个地址（0x%llx / 0x%llx，模块 %s）",
                      m_entries[i].id,
                      static_cast<unsigned long long>(m_entries[i - 1].address),
                      static_cast<unsigned long long>(m_entries[i].address),
                      m_entries[i].module.c_str());
        duplicates.emplace_back(buf);
    }
    return duplicates;
}

std::size_t SafepointTable::Size() const {
    EnsureLoaded();
    return m_entries.size();
}

std::vector<std::string> SafepointTable::ScanErrors() const {
    EnsureLoaded();
    return m_errors;
}

void SafepointTable::ClearForTest() {
    m_entries.clear();
    m_errors.clear();
    m_loaded = false;
}

void RefreshSafepoints() {
    SafepointTable::Instance().Refresh();
}

// ---------------------------------------------------------------------------
// 恢复侧状态
// ---------------------------------------------------------------------------
namespace {
std::uint32_t g_pending_id = 0;
std::int64_t g_pending_value = 0;
}  // namespace

void ArmSafepointResume(std::uint32_t id, std::int64_t value) {
    g_pending_id = id;
    g_pending_value = value;
}

bool SafepointResumePending(std::uint32_t id) {
    if (g_pending_id == 0 || g_pending_id != id) return false;
    g_pending_id = 0;
    return true;
}

std::int64_t SafepointResumeValue() {
    return g_pending_value;
}

void ClearSafepointResume() {
    g_pending_id = 0;
    g_pending_value = 0;
}

}  // namespace migration
