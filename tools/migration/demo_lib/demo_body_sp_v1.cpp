/**
 * @file demo_body_sp_v1.cpp
 * @brief v3 safepoint demo（布局版本 1）。
 *
 * 与 `demo_body_sp_v2.cpp` 语义完全一致，但**函数内指令布局不同**（v2 在
 * 挂起点前后插入了不同长度的无关代码）。快照里第一个挂起点记录的是
 * `safepoint id = 0x00020001`，恢复时按 id 查 v2 镜像里的锚点，因此指令偏移
 * 变化不影响 resume。
 *
 * 注意：本 demo 刻意保持**栈帧布局**（局部变量集合/顺序）与 v2 一致 ——
 * safepoint id 解决的是「同函数内指令偏移」，不是「栈帧结构跨版本变化」；
 * 后者需要重新映射整帧，见 docs/fiber-migration.md §8。
 *
 * 同时演示最小句柄表：
 * - `std::string*`（内建序列化）；
 * - POD `DemoPayload`（自定义 MigrationSerialize/Deserialize）。
 */
#include "migration/handles.hpp"
#include "migration/runtime.hpp"
#include "migration/safepoints.hpp"
#include "migration/symbol_table.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" const char migration_demo_version[] = "sp-v1";

namespace {

constexpr std::uint32_t kSafepointRecv1 = 0x00020001;
constexpr std::uint32_t kSafepointRecv2 = 0x00020002;
constexpr std::uint64_t kPayloadMagic = 0x475032435631ULL;
constexpr std::int32_t kPayloadId = 7;
constexpr char kLabelText[] = "跨进程标签";

struct DemoPayload {
    std::uint64_t magic;
    std::int32_t id;
    std::int32_t reserved;
    char text[24];
};

bool MigrationSerialize(const DemoPayload& value,
                        std::vector<std::uint8_t>* out) {
    out->clear();
    migration::wire::AppendU64(value.magic, out);
    migration::wire::AppendU32(static_cast<std::uint32_t>(value.id), out);
    migration::wire::AppendU32(static_cast<std::uint32_t>(value.reserved), out);
    migration::wire::AppendBytes(value.text, sizeof(value.text), out);
    return true;
}

bool MigrationDeserialize(DemoPayload* value, const std::uint8_t* data,
                          std::size_t size) {
    std::size_t pos = 0;
    std::uint64_t magic = 0;
    std::uint32_t id = 0;
    std::uint32_t reserved = 0;
    if (!migration::wire::ReadU64(data, size, &pos, &magic)) return false;
    if (!migration::wire::ReadU32(data, size, &pos, &id)) return false;
    if (!migration::wire::ReadU32(data, size, &pos, &reserved)) return false;
    const std::uint8_t* text = nullptr;
    if (!migration::wire::ReadBytes(data, size, &pos, sizeof(value->text), &text)) {
        return false;
    }
    value->magic = magic;
    value->id = static_cast<std::int32_t>(id);
    value->reserved = static_cast<std::int32_t>(reserved);
    std::memcpy(value->text, text, sizeof(value->text));
    return true;
}

MIGRATION_SERIALIZABLE("DemoPayload", DemoPayload);

}  // namespace

extern "C" __attribute__((noinline)) void migration_demo_body(
    migration::FiberCtl* self) {
    auto* const channel = reinterpret_cast<migration::ChannelCtl*>(self->channel);

    volatile unsigned char payload[64 * 1024];
    for (std::size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<unsigned char>((i * 31U + 7U) & 0xFFU);
    }
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) checksum += payload[i];
    self->arg = checksum;

    // 与 v2 相同大小的「帧布局锚」：两版都声明，保证帧大小/局部槽位一致。
    volatile unsigned char layout_pad[64];
    layout_pad[0] = 3;
    layout_pad[63] = 5;

    auto* label = new std::string(kLabelText);
    auto* heap = new DemoPayload{kPayloadMagic, kPayloadId, 0, {}};
    std::strncpy(heap->text, "heap-payload", sizeof(heap->text) - 1);
    // 默认登记；`MIGRATION_DEMO_UNREGISTERED_HEAP=1` 时故意不登记，
    // 用来验证「未登记堆指针必须拒绝」的负例。
    if (std::getenv("MIGRATION_DEMO_UNREGISTERED_HEAP") == nullptr) {
        MIGRATION_MIGRATE_OBJECT(label, "std::string");
        MIGRATION_MIGRATE_OBJECT(heap, "DemoPayload");
    }
    std::string* volatile keep_label = label;
    DemoPayload* volatile keep_heap = heap;
    migration::Log(
        "[sp-v1] keep slots: label=%p(&%p) heap=%p(&%p) slot_off=0x%llx/0x%llx\n",
        (void*)label, (void*)&keep_label, (void*)heap, (void*)&keep_heap,
        static_cast<unsigned long long>((std::uint64_t)&keep_label -
                                        self->stack_base),
        static_cast<unsigned long long>((std::uint64_t)&keep_heap -
                                        self->stack_base));

    migration::Log(
        "fiber#%llu [sp-v1] 启动，准备接收（payload=%zu 字节，校验和=%llu，"
        "pad=%u）\n",
        static_cast<unsigned long long>(self->id), sizeof(payload),
        static_cast<unsigned long long>(checksum),
        static_cast<unsigned>(layout_pad[0]));

    std::int64_t first = 0;
    MIGRATION_RECV(kSafepointRecv1, first, channel, self);
    migration::Log("fiber#%llu [sp-v1] 继续执行，收到消息 #1 = %lld\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<long long>(first));
    self->result = first;

    std::uint64_t restored = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) restored += payload[i];
    if (restored != checksum) {
        migration::Log("fiber#%llu [sp-v1] payload 校验失败：%llu != %llu\n",
                       static_cast<unsigned long long>(self->id),
                       static_cast<unsigned long long>(restored),
                       static_cast<unsigned long long>(checksum));
        self->state = static_cast<std::uint32_t>(migration::FiberState::kFailed);
        return;
    }

    if (*keep_label != kLabelText) {
        migration::Log("fiber#%llu [sp-v1] 句柄 std::string 校验失败（size=%zu）\n",
                       static_cast<unsigned long long>(self->id),
                       keep_label->size());
        self->state = static_cast<std::uint32_t>(migration::FiberState::kFailed);
        return;
    }
    if (keep_heap->magic != kPayloadMagic || keep_heap->id != kPayloadId ||
        std::strncmp(keep_heap->text, "heap-payload", sizeof(keep_heap->text)) != 0) {
        migration::Log("fiber#%llu [sp-v1] 句柄 DemoPayload 校验失败\n",
                       static_cast<unsigned long long>(self->id));
        self->state = static_cast<std::uint32_t>(migration::FiberState::kFailed);
        return;
    }
    migration::Log(
        "fiber#%llu [sp-v1] 栈 payload 校验通过（%llu），句柄校验通过：label=\"%s\" "
        "payload=%s\n",
        static_cast<unsigned long long>(self->id),
        static_cast<unsigned long long>(restored), keep_label->c_str(),
        keep_heap->text);

    std::int64_t second = 0;
    MIGRATION_RECV(kSafepointRecv2, second, channel, self);
    migration::Log("fiber#%llu [sp-v1] 继续执行，收到消息 #2 = %lld，主体结束\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<long long>(second));
    self->result = second;
}

MIGRATION_SYMBOL(migration_demo_body);
