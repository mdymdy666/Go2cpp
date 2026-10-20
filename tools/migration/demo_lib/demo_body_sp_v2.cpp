/**
 * @file demo_body_sp_v2.cpp
 * @brief v3 safepoint demo（布局版本 2）：语义同 v1，但**指令布局不同**。
 *
 * 与 v1 的差异故意放在挂起点前后，且**不改变栈帧布局**（局部变量集合/顺序
 * 完全一致）：
 * - 第一次 `MIGRATION_RECV` 之前，用不同循环展开写满 `layout_pad`；
 * - 第一次恢复之后、第二次挂起之前，再写一段 pad。
 *
 * 结果：`migration_demo_body` 内第一个挂起点的**指令偏移**与 v1 不同，但
 * safepoint 槽位偏移相同。若恢复仍按裸偏移（旧方案）就会跳到错误指令；
 * v3 按 safepoint id (`0x00020001`) 查当前镜像的锚点，因此不受影响。
 *
 * 为什么必须保持栈帧布局：快照恢复的是旧栈，v2 代码按自己的帧布局访问
 * 局部变量；帧大小/槽位一变，恢复后的栈就与代码预期不一致。这一点是
 * safepoint id 的边界，见 docs/fiber-migration.md §8。
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

extern "C" const char migration_demo_version[] = "sp-v2";

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

/// 额外符号：`MIGRATION_DEMO_EXTRA_SYMBOL=1` 时注册，用来演示恢复侧
/// 「符号表版本差异」的明细输出（只在 v2 SO 里存在）。
__attribute__((noinline)) void SpExtraSymbol() {}

struct ExtraSymbolRegistrar {
    ExtraSymbolRegistrar() {
        if (std::getenv("MIGRATION_DEMO_EXTRA_SYMBOL") != nullptr) {
            migration::SymbolTable::Instance().Add(
                "SpExtraSymbol", reinterpret_cast<const void*>(&SpExtraSymbol), 0,
                migration::kCategoryFunction, "SpExtraSymbol");
        }
    }
};

[[maybe_unused]] const ExtraSymbolRegistrar g_extra_symbol_registrar;

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

    // 与 v1 相同大小的「帧布局锚」：两版都声明，保证帧大小/局部槽位一致；
    // v2 用循环写满，v1 只写两个字节 —— 指令长度不同，帧布局相同。
    volatile unsigned char layout_pad[64];
    for (int i = 0; i < 64; i += 4) {
        layout_pad[i] = static_cast<unsigned char>(i + 1);
        layout_pad[i + 1] = static_cast<unsigned char>(i + 2);
        layout_pad[i + 2] = static_cast<unsigned char>(i + 3);
        layout_pad[i + 3] = static_cast<unsigned char>(i + 4);
    }

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
        "[sp-v2] keep slots: label=%p(&%p) heap=%p(&%p) slot_off=0x%llx/0x%llx\n",
        (void*)label, (void*)&keep_label, (void*)heap, (void*)&keep_heap,
        static_cast<unsigned long long>((std::uint64_t)&keep_label -
                                        self->stack_base),
        static_cast<unsigned long long>((std::uint64_t)&keep_heap -
                                        self->stack_base));

    migration::Log(
        "fiber#%llu [sp-v2] 启动，准备接收（payload=%zu 字节，校验和=%llu，"
        "pad=%u）\n",
        static_cast<unsigned long long>(self->id), sizeof(payload),
        static_cast<unsigned long long>(checksum),
        static_cast<unsigned>(layout_pad[0]));

    std::int64_t first = 0;
    MIGRATION_RECV(kSafepointRecv1, first, channel, self);
    migration::Log("fiber#%llu [sp-v2] 继续执行，收到消息 #1 = %lld\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<long long>(first));
    self->result = first;

    std::uint64_t restored = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) restored += payload[i];
    if (restored != checksum) {
        migration::Log("fiber#%llu [sp-v2] payload 校验失败：%llu != %llu\n",
                       static_cast<unsigned long long>(self->id),
                       static_cast<unsigned long long>(restored),
                       static_cast<unsigned long long>(checksum));
        self->state = static_cast<std::uint32_t>(migration::FiberState::kFailed);
        return;
    }

    if (*keep_label != kLabelText) {
        migration::Log("fiber#%llu [sp-v2] 句柄 std::string 校验失败（size=%zu）\n",
                       static_cast<unsigned long long>(self->id),
                       keep_label->size());
        self->state = static_cast<std::uint32_t>(migration::FiberState::kFailed);
        return;
    }
    if (keep_heap->magic != kPayloadMagic || keep_heap->id != kPayloadId ||
        std::strncmp(keep_heap->text, "heap-payload", sizeof(keep_heap->text)) != 0) {
        migration::Log("fiber#%llu [sp-v2] 句柄 DemoPayload 校验失败\n",
                       static_cast<unsigned long long>(self->id));
        self->state = static_cast<std::uint32_t>(migration::FiberState::kFailed);
        return;
    }
    migration::Log(
        "fiber#%llu [sp-v2] 栈 payload 校验通过（%llu），句柄校验通过：label=\"%s\" "
        "payload=%s\n",
        static_cast<unsigned long long>(self->id),
        static_cast<unsigned long long>(restored), keep_label->c_str(),
        keep_heap->text);

    // ---- 布局差异点 2：两次挂起之间再改一次 pad（不新增局部变量）----
    for (int i = 0; i < 64; i += 8) {
        layout_pad[i] ^= static_cast<unsigned char>(0x5A + i);
    }
    migration::Log("fiber#%llu [sp-v2] 布局扰动 B（pad[0]=%u）\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<unsigned>(layout_pad[0]));

    std::int64_t second = 0;
    MIGRATION_RECV(kSafepointRecv2, second, channel, self);
    migration::Log("fiber#%llu [sp-v2] 继续执行，收到消息 #2 = %lld，主体结束\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<long long>(second));
    self->result = second;
}

MIGRATION_SYMBOL(migration_demo_body);
