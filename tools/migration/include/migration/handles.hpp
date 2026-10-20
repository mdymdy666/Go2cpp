/**
 * @file handles.hpp
 * @brief 可迁移堆对象的句柄表（v3 最小实现）。
 *
 * ## 解决什么
 *
 * v2 遇到栈上指向堆的指针一律拒绝，因为堆地址跨进程没有意义。v3 的最小做法
 * 是**显式句柄化**：
 *
 * 1. 挂起之前，用户把要跟随 Fiber 迁移的堆对象登记进来：
 *    `MIGRATION_MIGRATE_OBJECT(ptr, "std::string")`；
 * 2. dump 扫描到栈上指针落在已登记对象区间内时，分配句柄 id，把**对象内容**
 *    （由该类型的序列化函数产出）写进快照的 Handles 段，栈槽改成
 *    `{句柄 id, 对象内偏移}` 重定位；
 * 3. restore 按类型名找到反序列化工厂，重建对象，把栈槽改成新地址 + 偏移。
 *
 * 没有登记、或类型没有提供序列化函数的堆指针，仍然**一律拒绝**。
 *
 * ## 类型契约
 *
 * 类型必须显式声明可序列化，提供 ADL 可找到的：
 *
 * ```cpp
 * bool MigrationSerialize(const DemoPayload& value,
 *                         std::vector<std::uint8_t>* out);
 * bool MigrationDeserialize(DemoPayload* value,
 *                           const std::uint8_t* data, std::size_t size);
 * MIGRATION_SERIALIZABLE("DemoPayload", DemoPayload);
 * ```
 *
 * 或者特化 `migration::MigrationCodec<T>`。内建支持 `std::string`。
 * 序列化格式由类型自己决定，但必须**自包含**：对象内部再有堆指针时，要么在
 * 序列化函数里展开，要么视为不支持 —— 本实现不会递归扫描对象内部。
 *
 * ## 所有权
 *
 * - dump 侧：对象仍归原进程所有，本实现不释放；
 * - restore 侧：重建对象由句柄表持有，`ReleaseHandles()` 统一析构。
 *   恢复出来的 Fiber 仍在运行时不得释放（Fiber 栈上还有指向它的指针）。
 *
 * ## 不支持
 *
 * - 循环引用 / 对象间指针（需要图遍历，未实现）；
 * - 非默认构造类型；
 * - 虚表指针、`shared_ptr` 控制块等运行时内部状态。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace migration {

/// 把对象序列化成字节串；返回 false 表示该对象不可迁移。
using SerializeObjectFn = bool (*)(const void* object,
                                   std::vector<std::uint8_t>* out);
/// 从字节串重建对象（新分配）；失败返回 false 且不得泄漏。
using DeserializeObjectFn = bool (*)(const std::uint8_t* data, std::size_t size,
                                     void** out);
/// 析构一个由 `DeserializeObjectFn` 创建的对象。
using DestroyObjectFn = void (*)(void* object);

/// 一种可迁移类型的工厂。
struct SerializerEntry {
    std::string type;
    SerializeObjectFn serialize{nullptr};
    DeserializeObjectFn deserialize{nullptr};
    DestroyObjectFn destroy{nullptr};
};

/// 注册（重复注册同名类型时保留第一个，避免静态初始化顺序歧义）。
void RegisterSerializer(const char* type, SerializeObjectFn serialize,
                        DeserializeObjectFn deserialize, DestroyObjectFn destroy);

/// 按类型名查找工厂；不存在返回 nullptr。
const SerializerEntry* FindSerializer(const std::string& type);

/// 当前已注册的类型名（诊断用，按名字排序）。
std::vector<std::string> RegisteredTypes();

// ---------------------------------------------------------------------------
// 编解码分派：用户可特化 MigrationCodec，或提供 ADL 的
// MigrationSerialize / MigrationDeserialize。
// ---------------------------------------------------------------------------

template <typename T>
struct MigrationCodec {
    static bool Serialize(const T& value, std::vector<std::uint8_t>* out) {
        return MigrationSerialize(value, out);
    }
    static bool Deserialize(T* value, const std::uint8_t* data,
                            std::size_t size) {
        return MigrationDeserialize(value, data, size);
    }
};

/// 内建：`std::string` 按「长度 + 字节」序列化。
template <>
struct MigrationCodec<std::string> {
    static bool Serialize(const std::string& value, std::vector<std::uint8_t>* out);
    static bool Deserialize(std::string* value, const std::uint8_t* data,
                            std::size_t size);
};

/// 内部：两段式拼接，保证 `__LINE__` 先展开再拼接。
#define MIGRATION_HANDLE_CONCAT_INNER(a, b) a##b
#define MIGRATION_HANDLE_CONCAT(a, b) MIGRATION_HANDLE_CONCAT_INNER(a, b)

/// 把一种类型注册进工厂表。`Name` 是裸字符串字面量，`Type` 可含逗号。
#define MIGRATION_SERIALIZABLE(Name, ...)                                    \
    [[maybe_unused]] static const ::migration::SerializerRegistrar<__VA_ARGS__> \
        MIGRATION_HANDLE_CONCAT(_migration_serializer_, __LINE__)(Name)

/// 类型注册器：构造即把编解码函数注册进全局表。
template <typename T>
class SerializerRegistrar {
public:
    explicit SerializerRegistrar(const char* type) {
        RegisterSerializer(
            type,
            [](const void* object, std::vector<std::uint8_t>* out) {
                return MigrationCodec<T>::Serialize(
                    *static_cast<const T*>(object), out);
            },
            [](const std::uint8_t* data, std::size_t size, void** out) -> bool {
                auto* object = new T();
                if (!MigrationCodec<T>::Deserialize(object, data, size)) {
                    delete object;
                    return false;
                }
                *out = object;
                return true;
            },
            [](void* object) { delete static_cast<T*>(object); });
    }
};

// ---------------------------------------------------------------------------
// dump 侧：对象登记
// ---------------------------------------------------------------------------

/// 一个已登记对象的区间。
struct MigratableObject {
    std::uint64_t address{0};
    std::uint64_t size{0};
    std::string type;
};

/**
 * @brief 登记一个「挂起时若栈上有它的指针就一起带走」的堆对象。
 *
 * 必须在 Fiber 挂起之前调用（典型位置：分配之后立即登记）。重复登记同一
 * 地址只保留第一次。对象区间为 `[object, object + size)`，允许栈上出现指向
 * 对象内部的指针（按内部偏移重写）。
 */
void RegisterMigratableObject(const void* object, std::size_t size,
                              const char* type);

/// 查找包含 `address` 的登记项，并给出对象内偏移；未登记返回 nullptr。
const MigratableObject* FindMigratableObject(std::uint64_t address,
                                             std::size_t* interior_offset);

/// 返回全部登记项（诊断用）。
std::vector<MigratableObject> MigratableObjects();

/// 清空登记表（仅测试用）。
void ClearMigratableObjectsForTest();

// ---------------------------------------------------------------------------
// restore 侧：句柄表
// ---------------------------------------------------------------------------

/// 安装一个重建出来的对象；重复 id 视为错误。
bool InstallHandle(std::uint32_t id, void* object, std::uint64_t size,
                   const char* type);

/// 按句柄 id 取对象；不存在返回 nullptr。
void* LookupHandle(std::uint32_t id, std::uint64_t* size);

/// 析构全部重建对象并清空句柄表；返回析构数量。
std::size_t ReleaseHandles();

/// 当前句柄数量。
std::size_t HandleCount();

// ---------------------------------------------------------------------------
// 线格式小工具（小端、定宽），供用户序列化函数复用
// ---------------------------------------------------------------------------
namespace wire {

void AppendU32(std::uint32_t value, std::vector<std::uint8_t>* out);
void AppendU64(std::uint64_t value, std::vector<std::uint8_t>* out);
void AppendBytes(const void* data, std::size_t size,
                 std::vector<std::uint8_t>* out);
bool ReadU32(const std::uint8_t* data, std::size_t size, std::size_t* pos,
             std::uint32_t* value);
bool ReadU64(const std::uint8_t* data, std::size_t size, std::size_t* pos,
             std::uint64_t* value);
bool ReadBytes(const std::uint8_t* data, std::size_t size, std::size_t* pos,
               std::size_t count, const std::uint8_t** out);

}  // namespace wire

}  // namespace migration

/**
 * @brief 把 `ptr` 指向的对象登记为可迁移。
 *
 * ```cpp
 * auto* label = new std::string("跨进程标签");
 * MIGRATION_MIGRATE_OBJECT(label, "std::string");
 * ```
 * 第二个参数是**类型名字符串字面量**，必须与恢复侧的
 * `MIGRATION_SERIALIZABLE` 注册名一致。
 */
#define MIGRATION_MIGRATE_OBJECT(ptr, type_name)                             \
    ::migration::RegisterMigratableObject((ptr), sizeof(*(ptr)), (type_name))
