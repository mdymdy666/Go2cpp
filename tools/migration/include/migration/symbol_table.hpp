/**
 * @file symbol_table.hpp
 * @brief 符号化迁移的符号注册表（v3：lambda + 冲突检测 + 版本哈希）。
 *
 * ## 为什么需要它
 *
 * 快照里保存裸地址的前提是「恢复进程的地址布局与生成进程一致」，这要求
 * 固定 VA 映射与 `-no-pie`。v2 起改为保存 `{符号名, 偏移}`，恢复时按名字在
 * 当前进程重新解析，于是 PIE / ASLR / SO 换版本都能工作。
 *
 * v3 在 v2 基础上补了三件事：
 *
 * 1. **lambda / 静态函数**：`MIGRATION_SYMBOL_FN(fn)` 通过 `dladdr` 或 ELF
 *    `.symtab` 找到函数指针的 mangled name，再用 `abi::__cxa_demangle`
 *    生成可读名作为稳定键；lambda 的 `operator()` 也能注册。
 * 2. **唯一性**：同名符号注册到不同地址（inline 克隆、手写重复注册）会被
 *    记录为冲突，dump 直接拒绝并打印两个地址。
 * 3. **版本哈希**：`Hash()` 覆盖整张表的符号名（不含地址），跨 SO 版本稳定；
 *    快照同时记录「引用集哈希」与「整表哈希」，恢复侧据此给出增删明细。
 *
 * ## 两条来源
 *
 * 1. **编译期注册（主路径）**：`MIGRATION_SYMBOL(name)` / 函数注册器在静态
 *    初始化期把 `{名字, 运行时地址}` 写进全局表；
 * 2. **dladdr 兜底**：注册表查不到时用 `dladdr` 取动态符号名，恢复时再用
 *    `dlsym(RTLD_DEFAULT, name)` 反查（要求 `-rdynamic` 或 SO 导出）。
 *
 * 注意：v3 **不会**用 `.symtab` 自动解析栈上任意地址 —— 那会绕过「显式注册」
 * 的约束。`.symtab` 只用于给 `MIGRATION_SYMBOL_FN` 找可读名。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace migration {

/// 符号类别（数值与 format.hpp 的 `SymbolKind` 一致）。
enum SymbolCategory : std::uint32_t {
    kCategoryUnknown = 0,
    kCategoryFunction = 1,
    kCategoryData = 2,
    kCategoryLambda = 3,
};

/// 一个可被符号化引用的地址区间（函数或全局对象）。
struct SymbolInfo {
    std::string name;     ///< 稳定键（注册名 / demangled 名）
    std::string display;  ///< 可读名（一般为 demangled）
    std::uint64_t address{0};
    std::uint64_t size{0};  ///< 0 表示未知，按「最近下界 + 最大跨度」匹配
    std::uint32_t kind{kCategoryUnknown};
};

/// 同名不同地址的冲突。
struct SymbolConflict {
    std::string name;
    std::uint64_t first_address{0};
    std::uint64_t second_address{0};
};

/// 进程级符号注册表（单例）。
class SymbolTable {
public:
    static SymbolTable& Instance();

    /// 注册一个符号。同名同地址忽略；同名不同地址记录冲突并保留第一个。
    void Add(const char* name, const void* address, std::uint64_t size,
             std::uint32_t kind = kCategoryUnknown, const char* display = nullptr);

    /// dump 方向：把运行时地址翻译成 `{name, offset}`。
    /// @return 解析成功返回 true；失败时 name 为空。
    bool ResolveAddress(std::uint64_t address, std::string* name,
                        std::int64_t* offset) const;

    /// dump 方向：同 `ResolveAddress`，额外返回符号类别与可读名。
    bool ResolveSymbol(std::uint64_t address, SymbolInfo* info,
                       std::int64_t* offset) const;

    /// restore 方向：把符号名解析成当前进程的运行时地址。
    bool Lookup(const std::string& name, std::uint64_t* address) const;

    /// 整表符号名集合哈希（排序后 FNV-1a），不含地址，跨进程/跨 SO 版本稳定。
    std::uint64_t Hash() const;

    /// 任意名字集合的哈希（快照的「引用集哈希」用它）。
    static std::uint64_t HashNames(const std::vector<std::string>& names);

    /// 返回当前全部符号（按地址排序），用于诊断。
    std::vector<SymbolInfo> Snapshot() const;

    /// 同名不同地址的冲突列表。
    std::vector<SymbolConflict> Conflicts() const;

    /// 诊断：解释一个无法符号化的地址，并给出「需要注册」的建议。
    std::string DiagnoseUnresolved(std::uint64_t address) const;

    /// 清空（仅测试用）。
    void Clear();

private:
    SymbolTable() = default;

    /// 注册项。注册发生在启动期，读取侧不加锁；`Add` 内部用互斥量串行化写入。
    mutable std::vector<SymbolInfo> m_entries;
    mutable std::vector<SymbolConflict> m_conflicts;
};

/// `MIGRATION_SYMBOL` 宏展开出的静态对象：构造即注册。
struct SymbolRegistrar {
    SymbolRegistrar(const char* name, const void* address, std::uint64_t size);
};

/// `MIGRATION_SYMBOL_FN` 宏展开出的静态对象：按函数指针注册。
struct FunctionSymbolRegistrar {
    /// @param address 函数指针；@param hint 源码级提示或显式名。
    /// @param explicit_name 为 true 时 hint 直接作为稳定键（不 demangle）。
    FunctionSymbolRegistrar(const void* address, const char* hint,
                            bool explicit_name = false);
};

/// 用 `dladdr` / ELF `.symtab` 解析函数指针的可读名并注册（供宏调用）。
void RegisterFunctionSymbol(const void* address, const char* hint,
                            bool explicit_name = false);

/// 返回函数指针对应的 demangled 名（找不到返回空串）。供诊断/测试复用。
std::string DemangleFunctionSymbol(const void* address);

/// 把函数指针 / 无捕获 lambda 归一成 `const void*`。
/// 普通函数指针走第一分支；lambda（闭包类型）走第二分支的 `+` 转换
/// （得到转换函数 `_FUN`，不是 `operator()`）。
template <typename Fn, typename std::enable_if<std::is_pointer<Fn>::value, int>::type = 0>
inline const void* NormalizeFunction(Fn fn) {
    return reinterpret_cast<const void*>(fn);
}

template <typename Fn, typename std::enable_if<!std::is_pointer<Fn>::value, int>::type = 0>
inline const void* NormalizeFunction(Fn fn) {
    return reinterpret_cast<const void*>(+fn);
}

/**
 * @brief 取无捕获 lambda 的 `operator()` 代码地址。
 *
 * 为什么需要它：`+lambda` 得到的是编译器生成的转换函数 `_FUN`，而真正
 * 出现在栈帧上的是 `operator()` 本体（`_FUN` 只是 `jmp` 过去）。只注册
 * `_FUN` 的话，栈上的 lambda 帧仍然无法符号化。
 *
 * 成员函数指针在 Itanium C++ ABI 下的前 8 字节就是非虚函数地址；这里按
 * 该 ABI 取地址（本项目只支持 Linux x86-64 / Itanium ABI）。非 lambda 类型
 * 返回 nullptr，注册器会忽略。
 */
template <typename Fn, typename std::enable_if<!std::is_pointer<Fn>::value, int>::type = 0>
inline const void* LambdaOperatorAddress(Fn& fn) {
    (void)fn;
    using Closure = typename std::remove_cv<Fn>::type;
    auto member = &Closure::operator();
    static_assert(sizeof(member) >= sizeof(void*), "成员函数指针布局不受支持");
    const void* address = nullptr;
    std::memcpy(&address, &member, sizeof(address));
    return address;
}

template <typename Fn, typename std::enable_if<std::is_pointer<Fn>::value, int>::type = 0>
inline const void* LambdaOperatorAddress(Fn&) {
    return nullptr;
}

}  // namespace migration

/**
 * 把 `name` 注册为可迁移符号。
 *
 * 用在「可能出现在 Fiber 栈上」的函数或全局对象上：
 * ```cpp
 * void FiberEntry(Transfer) noexcept;
 * MIGRATION_SYMBOL(FiberEntry);
 * ```
 * 宏必须写在目标声明之后、且与目标同作用域；`__attribute__((used))`
 * 防止链接器把没有任何引用的注册对象丢掉。
 */
#define MIGRATION_SYMBOL(name)                                              \
    [[gnu::used]] static ::migration::SymbolRegistrar _migration_symbol_##name( \
        #name, reinterpret_cast<const void*>(&(name)), 0)

/**
 * 把函数指针 / 无捕获 lambda 注册为可迁移符号（v3）。
 *
 * ```cpp
 * auto body = [](migration::FiberCtl* self) __attribute__((noinline)) {
 *     ...
 * };
 * MIGRATION_SYMBOL_FN(body);
 * ```
 *
 * - 只接受可转换为函数指针的对象（无捕获 lambda / 普通函数指针）；
 * - 目标必须 `noinline`，否则它不会以自己的符号出现在栈帧上；
 * - 稳定键优先取 demangled 名（如 `main::{lambda(FiberCtl*)#1}::operator()`），
 *   找不到时退回 `__PRETTY_FUNCTION__` 里的类型串；
 * - 注册发生在静态初始化期，必须在 Fiber 挂起前完成（通常写在定义处）。
 */
#define MIGRATION_SYMBOL_FN(fn)                                              \
    [[maybe_unused]] static const ::migration::FunctionSymbolRegistrar         \
        MIGRATION_G2C_CONCAT(_migration_fn_symbol_, __LINE__)(               \
            ::migration::NormalizeFunction(fn), #fn, false);                 \
    [[maybe_unused]] static const ::migration::FunctionSymbolRegistrar         \
        MIGRATION_G2C_CONCAT(_migration_fn_operator_, __LINE__)(             \
            ::migration::LambdaOperatorAddress(fn), #fn, false)

/**
 * @brief 同 `MIGRATION_SYMBOL_FN`，但显式指定稳定键。
 *
 * 用于符号表被 strip、或需要跨编译器/跨版本保持名字完全一致的场景：
 * ```cpp
 * MIGRATION_SYMBOL_FN_AS(body, "MyService::FiberBody");
 * ```
 */
#define MIGRATION_SYMBOL_FN_AS(fn, name)                                     \
    [[maybe_unused]] static const ::migration::FunctionSymbolRegistrar         \
        MIGRATION_G2C_CONCAT(_migration_fn_symbol_as_, __LINE__)(            \
            ::migration::NormalizeFunction(fn), (name), true);               \
    [[maybe_unused]] static const ::migration::FunctionSymbolRegistrar         \
        MIGRATION_G2C_CONCAT(_migration_fn_operator_as_, __LINE__)(          \
            ::migration::LambdaOperatorAddress(fn), (name) "::operator()",    \
            true)

/// 内部：两段式拼接，保证 `__LINE__` 先展开再拼接。
#define MIGRATION_G2C_CONCAT_INNER(a, b) a##b
#define MIGRATION_G2C_CONCAT(a, b) MIGRATION_G2C_CONCAT_INNER(a, b)
