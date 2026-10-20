/**
 * @file symbol_table.cpp
 * @brief 符号注册表实现（v3：demangle + 冲突检测 + 版本哈希）。
 */
#include "migration/symbol_table.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace migration {
namespace {

/// size 未知时，一个符号可覆盖的最大字节数（函数体上限的保守值）。
constexpr std::uint64_t kMaxUnknownSymbolSpan = 1ULL << 20;

std::uint64_t Fnv1a(const void* data, std::size_t size,
                    std::uint64_t seed = 0xCBF29CE484222325ULL) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

std::string DemangleCxa(const std::string& mangled) {
    if (mangled.empty() || mangled[0] != '_') return std::string();
    int status = 0;
    char* demangled = ::abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status != 0 || demangled == nullptr) {
        std::free(demangled);
        return std::string();
    }
    std::string text(demangled);
    std::free(demangled);
    return text;
}

/// 读整个 ELF 文件（上限 256 MiB）。
bool ReadElfFile(const std::string& path, std::vector<std::uint8_t>* out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0 ||
        static_cast<std::uint64_t>(st.st_size) > (256ULL << 20)) {
        ::close(fd);
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
    return got == out->size();
}

/// 在 `path` 的 `.symtab` / `.dynsym` 里按运行期地址找符号，返回 mangled 名与大小。
/// @param base 模块加载基址（ET_DYN 时 st_value 是相对基址的偏移）。
bool FindInSymtab(const std::string& path, std::uint64_t base,
                  std::uint64_t runtime_address, std::string* mangled,
                  std::uint64_t* size) {
    std::vector<std::uint8_t> elf;
    if (!ReadElfFile(path, &elf) || elf.size() < sizeof(Elf64_Ehdr)) return false;
    Elf64_Ehdr eh {};
    std::memcpy(&eh, elf.data(), sizeof(eh));
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }
    if (eh.e_shoff == 0 || eh.e_shnum == 0 ||
        eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shstrndx >= eh.e_shnum) {
        return false;
    }
    const std::uint64_t sh_end =
        eh.e_shoff + static_cast<std::uint64_t>(eh.e_shnum) * sizeof(Elf64_Shdr);
    if (sh_end > elf.size()) return false;

    const bool is_dyn = eh.e_type == ET_DYN;
    // .symtab 优先（含局部符号，lambda 的 operator() 在这里），其次 .dynsym。
    const Elf64_Word wanted[2] = {SHT_SYMTAB, SHT_DYNSYM};
    for (Elf64_Word sh_type : wanted) {
        for (std::uint16_t i = 0; i < eh.e_shnum; ++i) {
            Elf64_Shdr sh {};
            std::memcpy(&sh,
                        elf.data() + eh.e_shoff +
                            static_cast<std::uint64_t>(i) * sizeof(Elf64_Shdr),
                        sizeof(sh));
            if (sh.sh_type != sh_type) continue;
            if (sh.sh_entsize != sizeof(Elf64_Sym) || sh.sh_link >= eh.e_shnum) continue;
            Elf64_Shdr str_sh {};
            std::memcpy(&str_sh,
                        elf.data() + eh.e_shoff +
                            static_cast<std::uint64_t>(sh.sh_link) * sizeof(Elf64_Shdr),
                        sizeof(str_sh));
            if (sh.sh_offset + sh.sh_size > elf.size() ||
                str_sh.sh_offset + str_sh.sh_size > elf.size()) {
                continue;
            }
            const char* strings =
                reinterpret_cast<const char*>(elf.data() + str_sh.sh_offset);
            const std::uint64_t count = sh.sh_size / sizeof(Elf64_Sym);
            for (std::uint64_t k = 0; k < count; ++k) {
                Elf64_Sym sym {};
                std::memcpy(&sym, elf.data() + sh.sh_offset + k * sizeof(Elf64_Sym),
                            sizeof(sym));
                const int type = ELF64_ST_TYPE(sym.st_info);
                if (type != STT_FUNC && type != STT_GNU_IFUNC) continue;
                if (sym.st_shndx == SHN_UNDEF || sym.st_name >= str_sh.sh_size) continue;
                const std::uint64_t address =
                    is_dyn ? base + sym.st_value : sym.st_value;
                if (address != runtime_address) continue;
                const char* name = strings + sym.st_name;
                if (name[0] == '\0') continue;
                *mangled = name;
                *size = sym.st_size;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

SymbolTable& SymbolTable::Instance() {
    static SymbolTable table;
    return table;
}

void SymbolTable::Add(const char* name, const void* address, std::uint64_t size,
                      std::uint32_t kind, const char* display) {
    if (name == nullptr || name[0] == '\0' || address == nullptr) return;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const auto runtime_address = reinterpret_cast<std::uint64_t>(address);
    for (const auto& entry : m_entries) {
        if (entry.name != name) continue;
        if (entry.address == runtime_address) return;  // 同名同址：幂等
        // 同名不同址：inline 克隆 / 手写重复注册。记录冲突，保留第一个。
        SymbolConflict conflict;
        conflict.name = name;
        conflict.first_address = entry.address;
        conflict.second_address = runtime_address;
        for (const auto& existing : m_conflicts) {
            if (existing.name == conflict.name &&
                existing.first_address == conflict.first_address &&
                existing.second_address == conflict.second_address) {
                return;
            }
        }
        m_conflicts.push_back(std::move(conflict));
        return;
    }
    SymbolInfo info;
    info.name = name;
    info.display = display != nullptr ? display : "";
    info.address = runtime_address;
    info.size = size;
    info.kind = kind;
    m_entries.push_back(std::move(info));
}

bool SymbolTable::ResolveAddress(std::uint64_t address, std::string* name,
                                 std::int64_t* offset) const {
    SymbolInfo info;
    std::int64_t parsed_offset = 0;
    if (!ResolveSymbol(address, &info, &parsed_offset)) return false;
    if (name != nullptr) *name = info.name;
    if (offset != nullptr) *offset = parsed_offset;
    return true;
}

bool SymbolTable::ResolveSymbol(std::uint64_t address, SymbolInfo* info,
                                std::int64_t* offset) const {
    if (address == 0 || info == nullptr || offset == nullptr) return false;
    // 1) 精确区间匹配（注册时给了 size）。
    for (const auto& entry : m_entries) {
        if (entry.size == 0) continue;
        if (address >= entry.address && address < entry.address + entry.size) {
            *info = entry;
            *offset = static_cast<std::int64_t>(address - entry.address);
            return true;
        }
    }
    // 2) dladdr：动态符号表里的地址最精确（-rdynamic / SO 导出）。
    {
        Dl_info dl{};
        if (::dladdr(reinterpret_cast<void*>(address), &dl) != 0 &&
            dl.dli_sname != nullptr && dl.dli_saddr != nullptr) {
            info->name = dl.dli_sname;
            info->display = DemangleCxa(info->name);
            info->address = reinterpret_cast<std::uint64_t>(dl.dli_saddr);
            info->size = 0;
            info->kind = kCategoryFunction;
            *offset = static_cast<std::int64_t>(
                address - reinterpret_cast<std::uint64_t>(dl.dli_saddr));
            return true;
        }
    }
    // 3) 注册表兜底：按地址排序，只把地址归给「最近的、且下一个符号之前」
    //    的条目，避免跨符号误判；最后一个符号允许 1 MiB 的保守跨度。
    const SymbolInfo* best = nullptr;
    for (const auto& entry : m_entries) {
        if (entry.address > address) continue;
        if (best == nullptr || entry.address > best->address) best = &entry;
    }
    if (best == nullptr) return false;
    std::uint64_t next_address = 0;
    for (const auto& entry : m_entries) {
        if (entry.address <= best->address) continue;
        if (next_address == 0 || entry.address < next_address) next_address = entry.address;
    }
    // 只允许「最近下界符号 + 最大跨度」以内的地址。v2 曾把
    // `address < next_symbol` 当作充分条件，于是两个相距很远的符号之间
    // （例如主程序数据段与 dlopen 的 SO 之间）的堆地址会被误判成前一个
    // 符号的偏移 —— 堆指针因此绕过句柄检查。v3 一律要求跨度有界。
    const bool within_next = next_address != 0 && address < next_address;
    const bool within_span = address - best->address < kMaxUnknownSymbolSpan;
    if (within_next && within_span) {
        *info = *best;
        *offset = static_cast<std::int64_t>(address - best->address);
        return true;
    }
    return false;
}

bool SymbolTable::Lookup(const std::string& name, std::uint64_t* address) const {
    if (address == nullptr || name.empty()) return false;
    for (const auto& entry : m_entries) {
        if (entry.name == name) {
            *address = entry.address;
            return true;
        }
    }
    // 兜底：动态符号查找（注册表缺失时，例如符号来自未注册的 SO）。
    void* found = ::dlsym(RTLD_DEFAULT, name.c_str());
    if (found != nullptr) {
        *address = reinterpret_cast<std::uint64_t>(found);
        return true;
    }
    return false;
}

std::uint64_t SymbolTable::Hash() const {
    std::vector<std::string> names;
    names.reserve(m_entries.size());
    for (const auto& entry : m_entries) names.push_back(entry.name);
    return HashNames(names);
}

std::uint64_t SymbolTable::HashNames(const std::vector<std::string>& names) {
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const auto& name : sorted) {
        h = Fnv1a(name.data(), name.size(), h);
        const char sep = '\n';
        h = Fnv1a(&sep, 1, h);
    }
    return h;
}

std::vector<SymbolInfo> SymbolTable::Snapshot() const {
    std::vector<SymbolInfo> copy = m_entries;
    std::sort(copy.begin(), copy.end(), [](const SymbolInfo& a, const SymbolInfo& b) {
        return a.address < b.address;
    });
    return copy;
}

std::vector<SymbolConflict> SymbolTable::Conflicts() const {
    return m_conflicts;
}

std::string SymbolTable::DiagnoseUnresolved(std::uint64_t address) const {
    Dl_info dl{};
    std::string module = "未知模块";
    if (::dladdr(reinterpret_cast<void*>(address), &dl) != 0 &&
        dl.dli_fname != nullptr) {
        module = dl.dli_fname;
    }
    char buffer[768];
    std::snprintf(
        buffer, sizeof(buffer),
        "地址 0x%llx（模块 %s）无法符号化：该返回地址属于未注册的静态函数、"
        "匿名命名空间函数或 lambda，且不在动态符号表里。请在定义处加 "
        "MIGRATION_SYMBOL(name)（普通函数/全局对象）或 MIGRATION_SYMBOL_FN(fn)"
        "（lambda/函数指针），并确保目标带 noinline。",
        static_cast<unsigned long long>(address), module.c_str());
    return buffer;
}

void SymbolTable::Clear() {
    m_entries.clear();
    m_conflicts.clear();
}

SymbolRegistrar::SymbolRegistrar(const char* name, const void* address,
                                 std::uint64_t size) {
    SymbolTable::Instance().Add(name, address, size);
}

FunctionSymbolRegistrar::FunctionSymbolRegistrar(const void* address,
                                                 const char* hint,
                                                 bool explicit_name) {
    RegisterFunctionSymbol(address, hint, explicit_name);
}

std::string DemangleFunctionSymbol(const void* address) {
    if (address == nullptr) return std::string();
    const auto runtime_address = reinterpret_cast<std::uint64_t>(address);
    Dl_info dl{};
    const bool have_info = ::dladdr(address, &dl) != 0;
    if (have_info && dl.dli_sname != nullptr && dl.dli_saddr != nullptr &&
        reinterpret_cast<std::uint64_t>(dl.dli_saddr) == runtime_address) {
        return DemangleCxa(dl.dli_sname);
    }
    if (have_info && dl.dli_fname != nullptr) {
        std::string mangled;
        std::uint64_t size = 0;
        if (FindInSymtab(dl.dli_fname, reinterpret_cast<std::uint64_t>(dl.dli_fbase),
                         runtime_address, &mangled, &size)) {
            return DemangleCxa(mangled);
        }
    }
    return std::string();
}

void RegisterFunctionSymbol(const void* address, const char* hint,
                            bool explicit_name) {
    if (address == nullptr) return;
    const auto runtime_address = reinterpret_cast<std::uint64_t>(address);
    Dl_info dl{};
    const bool have_info = ::dladdr(address, &dl) != 0;
    if (explicit_name && hint != nullptr && hint[0] != '\0') {
        SymbolTable::Instance().Add(hint, address, 0, kCategoryFunction, hint);
        return;
    }
    std::string mangled;
    std::uint64_t size = 0;
    if (have_info && dl.dli_sname != nullptr && dl.dli_saddr != nullptr &&
        reinterpret_cast<std::uint64_t>(dl.dli_saddr) == runtime_address) {
        mangled = dl.dli_sname;
    }
    if (mangled.empty() && have_info && dl.dli_fname != nullptr) {
        FindInSymtab(dl.dli_fname, reinterpret_cast<std::uint64_t>(dl.dli_fbase),
                     runtime_address, &mangled, &size);
    }
    std::string display = mangled.empty() ? std::string() : DemangleCxa(mangled);
    std::string key = !display.empty() ? display : mangled;
    if (key.empty()) {
        // 符号表被 strip：退回源码级提示。名字仍要求跨构建一致，
        // 因此提示里不带地址。
        key = (hint != nullptr && hint[0] != '\0') ? hint : "lambda";
    }
    const bool is_lambda = key.find("lambda") != std::string::npos;
    SymbolTable::Instance().Add(
        key.c_str(), address, size,
        is_lambda ? kCategoryLambda : kCategoryFunction,
        display.empty() ? key.c_str() : display.c_str());
}

}  // namespace migration
