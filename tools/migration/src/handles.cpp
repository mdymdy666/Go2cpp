/**
 * @file handles.cpp
 * @brief 句柄表与类型工厂的实现。
 *
 * 两个互不相关的进程内状态：
 * - dump 侧登记「哪些堆对象跟着走」（`g_objects`）；
 * - restore 侧持有「重建出来的对象」（`g_handles`）。
 *
 * 单线程 demo 不额外加锁；注册发生在挂起/恢复之前，读取发生在之后。
 */
#include "migration/handles.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace migration {
namespace {

std::vector<SerializerEntry>& Serializers() {
    static std::vector<SerializerEntry> serializers;
    return serializers;
}

std::vector<MigratableObject>& Objects() {
    static std::vector<MigratableObject> objects;
    return objects;
}

struct HandleSlot {
    std::uint32_t id{0};
    std::uint64_t address{0};
    std::uint64_t size{0};
    std::string type;
    DestroyObjectFn destroy{nullptr};
};

std::vector<HandleSlot>& Handles() {
    static std::vector<HandleSlot> handles;
    return handles;
}

/// 内建 `std::string` 工厂：库初始化时自动注册。
const bool kStringSerializerRegistered = [] {
    RegisterSerializer(
        "std::string",
        [](const void* object, std::vector<std::uint8_t>* out) {
            return MigrationCodec<std::string>::Serialize(
                *static_cast<const std::string*>(object), out);
        },
        [](const std::uint8_t* data, std::size_t size, void** out) -> bool {
            auto* value = new std::string();
            if (!MigrationCodec<std::string>::Deserialize(value, data, size)) {
                delete value;
                return false;
            }
            *out = value;
            return true;
        },
        [](void* object) { delete static_cast<std::string*>(object); });
    return true;
}();


}  // namespace

// ---------------------------------------------------------------------------
// 类型工厂
// ---------------------------------------------------------------------------
void RegisterSerializer(const char* type, SerializeObjectFn serialize,
                        DeserializeObjectFn deserialize, DestroyObjectFn destroy) {
    if (type == nullptr || type[0] == '\0' || serialize == nullptr ||
        deserialize == nullptr || destroy == nullptr) {
        return;
    }
    for (const auto& entry : Serializers()) {
        if (entry.type == type) return;  // 同名保留第一个
    }
    SerializerEntry entry;
    entry.type = type;
    entry.serialize = serialize;
    entry.deserialize = deserialize;
    entry.destroy = destroy;
    Serializers().push_back(std::move(entry));
}

const SerializerEntry* FindSerializer(const std::string& type) {
    for (const auto& entry : Serializers()) {
        if (entry.type == type) return &entry;
    }
    return nullptr;
}

std::vector<std::string> RegisteredTypes() {
    std::vector<std::string> types;
    types.reserve(Serializers().size());
    for (const auto& entry : Serializers()) types.push_back(entry.type);
    std::sort(types.begin(), types.end());
    return types;
}

// ---------------------------------------------------------------------------
// dump 侧对象登记
// ---------------------------------------------------------------------------
void RegisterMigratableObject(const void* object, std::size_t size,
                              const char* type) {
    if (object == nullptr || size == 0 || type == nullptr || type[0] == '\0') {
        return;
    }
    const auto address = reinterpret_cast<std::uint64_t>(object);
    for (const auto& entry : Objects()) {
        if (entry.address == address) return;
    }
    MigratableObject entry;
    entry.address = address;
    entry.size = size;
    entry.type = type;
    Objects().push_back(std::move(entry));
}

const MigratableObject* FindMigratableObject(std::uint64_t address,
                                             std::size_t* interior_offset) {
    const MigratableObject* best = nullptr;
    for (const auto& entry : Objects()) {
        if (address < entry.address || address >= entry.address + entry.size) {
            continue;
        }
        // 区间嵌套时取范围最小的（更具体）那个。
        if (best == nullptr || entry.size < best->size) best = &entry;
    }
    if (best == nullptr) return nullptr;
    if (interior_offset != nullptr) {
        *interior_offset = static_cast<std::size_t>(address - best->address);
    }
    return best;
}

std::vector<MigratableObject> MigratableObjects() {
    return Objects();
}

void ClearMigratableObjectsForTest() {
    Objects().clear();
}

// ---------------------------------------------------------------------------
// restore 侧句柄表
// ---------------------------------------------------------------------------
bool InstallHandle(std::uint32_t id, void* object, std::uint64_t size,
                   const char* type) {
    if (id == 0 || object == nullptr) return false;
    for (const auto& slot : Handles()) {
        if (slot.id == id) return false;  // 重复安装是损坏信号
    }
    HandleSlot slot;
    slot.id = id;
    slot.address = reinterpret_cast<std::uint64_t>(object);
    slot.size = size;
    slot.type = type != nullptr ? type : "";
    const SerializerEntry* entry = FindSerializer(slot.type);
    slot.destroy = entry != nullptr ? entry->destroy : nullptr;
    Handles().push_back(std::move(slot));
    return true;
}

void* LookupHandle(std::uint32_t id, std::uint64_t* size) {
    for (const auto& slot : Handles()) {
        if (slot.id != id) continue;
        if (size != nullptr) *size = slot.size;
        return reinterpret_cast<void*>(slot.address);
    }
    return nullptr;
}

std::size_t ReleaseHandles() {
    std::size_t released = 0;
    for (auto it = Handles().rbegin(); it != Handles().rend(); ++it) {
        if (it->destroy != nullptr) {
            it->destroy(reinterpret_cast<void*>(it->address));
            ++released;
        }
    }
    Handles().clear();
    return released;
}

std::size_t HandleCount() {
    return Handles().size();
}

// ---------------------------------------------------------------------------
// 内建 std::string 编解码
// ---------------------------------------------------------------------------
bool MigrationCodec<std::string>::Serialize(const std::string& value,
                                            std::vector<std::uint8_t>* out) {
    if (out == nullptr) return false;
    out->clear();
    wire::AppendU64(static_cast<std::uint64_t>(value.size()), out);
    wire::AppendBytes(value.data(), value.size(), out);
    return true;
}

bool MigrationCodec<std::string>::Deserialize(std::string* value,
                                              const std::uint8_t* data,
                                              std::size_t size) {
    if (value == nullptr) return false;
    std::size_t pos = 0;
    std::uint64_t length = 0;
    if (!wire::ReadU64(data, size, &pos, &length)) return false;
    if (length > size - pos) return false;
    const std::uint8_t* bytes = nullptr;
    if (!wire::ReadBytes(data, size, &pos, static_cast<std::size_t>(length),
                         &bytes)) {
        return false;
    }
    value->assign(reinterpret_cast<const char*>(bytes),
                  static_cast<std::size_t>(length));
    return true;
}

// ---------------------------------------------------------------------------
// 线格式工具
// ---------------------------------------------------------------------------
namespace wire {

void AppendU32(std::uint32_t value, std::vector<std::uint8_t>* out) {
    for (int i = 0; i < 4; ++i) {
        out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

void AppendU64(std::uint64_t value, std::vector<std::uint8_t>* out) {
    for (int i = 0; i < 8; ++i) {
        out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

void AppendBytes(const void* data, std::size_t size,
                 std::vector<std::uint8_t>* out) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

bool ReadU32(const std::uint8_t* data, std::size_t size, std::size_t* pos,
             std::uint32_t* value) {
    if (pos == nullptr || *pos + 4 > size) return false;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(data[*pos + i]) << (8 * i);
    }
    *pos += 4;
    *value = v;
    return true;
}

bool ReadU64(const std::uint8_t* data, std::size_t size, std::size_t* pos,
             std::uint64_t* value) {
    if (pos == nullptr || *pos + 8 > size) return false;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(data[*pos + i]) << (8 * i);
    }
    *pos += 8;
    *value = v;
    return true;
}

bool ReadBytes(const std::uint8_t* data, std::size_t size, std::size_t* pos,
               std::size_t count, const std::uint8_t** out) {
    if (pos == nullptr || *pos + count > size) return false;
    *out = data + *pos;
    *pos += count;
    return true;
}

}  // namespace wire

}  // namespace migration
