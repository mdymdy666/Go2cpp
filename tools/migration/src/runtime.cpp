/**
 * @file runtime.cpp
 * @brief 可迁移协程最小运行时（v3）的实现。
 *
 * 关键流程：
 * 1. 栈用普通 mmap 分配，不再固定 VA；
 * 2. dump 时把代码/数据地址翻译成 `{符号名, 偏移}`，用户函数内的挂起点翻译
 *    成 `{safepoint id}`，栈上已登记堆对象翻译成 `{handle id, 内部偏移}`；
 * 3. restore 时按符号解析新地址、按 safepoint id 查当前镜像的锚点、按句柄
 *    重建堆对象，再逐个重定位栈槽并重建控制块。
 */
#include "migration/runtime.hpp"

#include "go2cpp/concurrency/detail/context_backend.hpp"
#include "migration/format.hpp"
#include "migration/handles.hpp"
#include "migration/safepoints.hpp"
#include "migration/signature.hpp"
#include "migration/symbol_table.hpp"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

namespace migration {

FiberCtl g_fiber_ctl{};
ChannelCtl g_channel_ctl{};

namespace {

using go2cpp::detail::JumpContext;
using go2cpp::detail::MakeContext;
using go2cpp::detail::Transfer;

/// 当前进程角色，仅用于日志前缀（A = dump 进程，B = restore 进程）。
const char* g_role = "A";
/// demo 主体：nullptr 表示使用可执行文件内建的实现。
DemoBodyFn g_demo_body = nullptr;

/// 可迁移函数必须禁止内联：rbp 链要能逐个帧遍历、逐个返回地址符号化。
#define MIGRATION_NOINLINE __attribute__((noinline))

/// 内建主体的挂起点编号（高 16 位 0x0001 表示「主程序内建 demo」）。
constexpr std::uint32_t kSafepointBuiltinRecv1 = 0x00010001;
constexpr std::uint32_t kSafepointBuiltinRecv2 = 0x00010002;

MIGRATION_NOINLINE void FiberEntry(Transfer transfer) noexcept;
MIGRATION_NOINLINE void ParkSelf(FiberCtl* self) noexcept;
MIGRATION_NOINLINE void ResumeFiber(FiberCtl* self) noexcept;
MIGRATION_NOINLINE void BuiltinDemoBody(FiberCtl* self);
MIGRATION_NOINLINE std::int64_t WaitForValue(ChannelCtl* channel, FiberCtl* self);

// 这些函数可能出现在 Fiber 栈的 rbp 链上，必须注册为可迁移符号。
// 匿名的 FiberEntry/BuiltinDemoBody 只能靠注册表解析；导出的
// ChannelRecv/ChannelRecvAt 还能被 dladdr 兜底。
MIGRATION_SYMBOL(FiberEntry);
MIGRATION_SYMBOL(ParkSelf);
MIGRATION_SYMBOL(ResumeFiber);
MIGRATION_SYMBOL(BuiltinDemoBody);
MIGRATION_SYMBOL(ChannelRecv);
MIGRATION_SYMBOL(ChannelRecvAt);
MIGRATION_SYMBOL(ParkAt);
MIGRATION_SYMBOL(g_fiber_ctl);
MIGRATION_SYMBOL(g_channel_ctl);

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
}  // namespace

void Log(const char* format, ...) {
    std::printf("[%s] ", g_role);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

namespace {

// ---------------------------------------------------------------------------
// 上下文切换
// ---------------------------------------------------------------------------
MIGRATION_NOINLINE void FiberEntry(Transfer transfer) noexcept {
    auto* const self = static_cast<FiberCtl*>(const_cast<void*>(transfer.data));
    self->caller = reinterpret_cast<std::uint64_t>(transfer.context);
    auto* const fn = reinterpret_cast<DemoBodyFn>(self->fn);
    if (fn != nullptr) {
        fn(self);
    }
    if (self->state != static_cast<std::uint32_t>(FiberState::kFailed)) {
        self->state = static_cast<std::uint32_t>(FiberState::kCompleted);
    }
    JumpContext(reinterpret_cast<void*>(self->caller), self);
    std::fprintf(stderr, "fiber entry 非法返回\n");
    std::abort();
}

/// 挂起当前 Fiber，控制权回到 `self->caller`；恢复后刷新 caller。
MIGRATION_NOINLINE void ParkSelf(FiberCtl* self) noexcept {
    const Transfer transfer = JumpContext(reinterpret_cast<void*>(self->caller), self);
    self->caller = reinterpret_cast<std::uint64_t>(transfer.context);
    self->state = static_cast<std::uint32_t>(FiberState::kRunning);
}

/// 恢复 `self`，直到它再次挂起或完成。
MIGRATION_NOINLINE void ResumeFiber(FiberCtl* self) noexcept {
    self->state = static_cast<std::uint32_t>(FiberState::kRunning);
    const Transfer transfer = JumpContext(reinterpret_cast<void*>(self->ctx), self);
    const auto state = self->state;
    if (state != static_cast<std::uint32_t>(FiberState::kCompleted) &&
        state != static_cast<std::uint32_t>(FiberState::kFailed)) {
        self->ctx = reinterpret_cast<std::uint64_t>(transfer.context);
    }
}

// ---------------------------------------------------------------------------
// 通道
// ---------------------------------------------------------------------------
MIGRATION_NOINLINE std::int64_t WaitForValue(ChannelCtl* channel, FiberCtl* self) {
    // 等待节点在栈上：随栈快照一起迁移。
    Waiter waiter{};
    waiter.next = 0;
    waiter.owner = reinterpret_cast<std::uint64_t>(self);
    waiter.value = 0;
    waiter.ready = 0;
    if (channel->tail != 0) {
        reinterpret_cast<Waiter*>(channel->tail)->next =
            reinterpret_cast<std::uint64_t>(&waiter);
    } else {
        channel->head = reinterpret_cast<std::uint64_t>(&waiter);
    }
    channel->tail = reinterpret_cast<std::uint64_t>(&waiter);
    self->channel = reinterpret_cast<std::uint64_t>(channel);
    self->waiter = reinterpret_cast<std::uint64_t>(&waiter);
    self->state = static_cast<std::uint32_t>(FiberState::kParked);
    while (waiter.ready == 0) {
        ParkSelf(self);
    }
    self->waiter = 0;
    self->channel = 0;
    return waiter.value;
}

/// 向通道发送一个值；有等待者时把值写入其节点并恢复对应 Fiber。
bool ChannelSend(ChannelCtl* channel, std::int64_t value, std::string* error) {
    if (channel->head == 0) {
        *error = "通道上没有等待者（demo 只支持无缓冲的阻塞收发）";
        return false;
    }
    auto* const waiter = reinterpret_cast<Waiter*>(channel->head);
    channel->head = waiter->next;
    if (channel->head == 0) {
        channel->tail = 0;
    }
    waiter->value = value;
    waiter->ready = 1;
    auto* const owner = reinterpret_cast<FiberCtl*>(waiter->owner);
    // v3：告诉恢复落点「本次 resume 对应哪个 safepoint、投递了什么值」。
    ArmSafepointResume(owner->safepoint_id, value);
    ResumeFiber(owner);
    return true;
}

// ---------------------------------------------------------------------------
// Fiber 主体（内建实现）
// ---------------------------------------------------------------------------
MIGRATION_NOINLINE void BuiltinDemoBody(FiberCtl* self) {
    auto* const channel = reinterpret_cast<ChannelCtl*>(self->channel);
    // 栈上放 64 KiB 可校验数据：dump 的栈快照必须把它完整带到另一个进程。
    volatile unsigned char payload[64 * 1024];
    for (std::size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<unsigned char>((i * 31U + 7U) & 0xFFU);
    }
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) checksum += payload[i];
    self->arg = checksum;

    Log("fiber#%llu 启动，准备在通道 0x%llx 上接收（栈上 payload=%zu 字节，校验和=%llu）\n",
        static_cast<unsigned long long>(self->id),
        static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(channel)),
        sizeof(payload), static_cast<unsigned long long>(checksum));

    std::int64_t first = 0;
    MIGRATION_RECV(kSafepointBuiltinRecv1, first, channel, self);
    Log("fiber#%llu 继续执行，收到消息 #1 = %lld\n",
        static_cast<unsigned long long>(self->id), static_cast<long long>(first));
    self->result = first;

    std::uint64_t restored = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) restored += payload[i];
    if (restored != checksum) {
        Log("fiber#%llu payload 校验失败：%llu != %llu\n",
            static_cast<unsigned long long>(self->id),
            static_cast<unsigned long long>(restored),
            static_cast<unsigned long long>(checksum));
        self->state = static_cast<std::uint32_t>(FiberState::kFailed);
        return;
    }
    Log("fiber#%llu 栈上 payload 校验通过（%llu）\n",
        static_cast<unsigned long long>(self->id),
        static_cast<unsigned long long>(restored));

    std::int64_t second = 0;
    MIGRATION_RECV(kSafepointBuiltinRecv2, second, channel, self);
    Log("fiber#%llu 继续执行，收到消息 #2 = %lld，主体结束\n",
        static_cast<unsigned long long>(self->id), static_cast<long long>(second));
    self->result = second;
}

DemoBodyFn ActiveDemoBody() {
    return g_demo_body != nullptr ? g_demo_body : &BuiltinDemoBody;
}

// ---------------------------------------------------------------------------
// 地址分类
// ---------------------------------------------------------------------------
struct MapRange {
    std::uint64_t lo{0};
    std::uint64_t hi{0};
    bool exec{false};
    bool heap{false};
    bool writable{false};
    bool anonymous{false};
};

std::vector<MapRange> ReadSelfMaps() {
    std::vector<MapRange> ranges;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long lo = 0;
        unsigned long long hi = 0;
        char perms[8] = {};
        int consumed = 0;
        const int n = std::sscanf(line.c_str(), "%llx-%llx %7s %*s %*s %*s %n", &lo, &hi,
                                  perms, &consumed);
        if (n < 3) continue;
        MapRange r{};
        r.lo = lo;
        r.hi = hi;
        r.exec = perms[2] == 'x';
        r.writable = perms[1] == 'w';
        r.heap = std::strstr(line.c_str(), "[heap]") != nullptr;
        const bool has_path = consumed > 0 &&
                              static_cast<std::size_t>(consumed) < line.size() &&
                              line[consumed] != '\0';
        r.anonymous = !has_path;
        ranges.push_back(r);
    }
    return ranges;
}

bool InMappings(const std::vector<MapRange>& ranges, std::uint64_t value,
                bool* is_exec, bool* is_heap) {
    for (const auto& range : ranges) {
        if (value < range.lo || value >= range.hi) continue;
        *is_exec = range.exec;
        *is_heap = range.heap ||
                   (range.writable && range.anonymous);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 快照组装（dump 方向）
// ---------------------------------------------------------------------------
struct DumpResult {
    std::uint64_t old_base{0};
    std::uint64_t old_top{0};
    std::uint64_t ctx{0};
};

/// 符号目录构建器：按名字去重，分配从 1 开始的 id。
class SymbolCatalog {
public:
    std::uint32_t Intern(const SymbolInfo& info) {
        for (const auto& entry : m_records) {
            if (entry.name == info.name) return entry.id;
        }
        SymbolRecord record{};
        record.id = static_cast<std::uint32_t>(m_records.size()) + 1;
        record.kind = info.kind;
        record.size = info.size;
        record.name = info.name;
        record.display = info.display.empty() ? info.name : info.display;
        m_records.push_back(std::move(record));
        return m_records.back().id;
    }

    /// 把地址翻译成符号引用。失败返回 0。
    std::uint32_t InternAddress(std::uint64_t address, SymbolRef* ref) {
        SymbolInfo info;
        std::int64_t offset = 0;
        if (!SymbolTable::Instance().ResolveSymbol(address, &info, &offset)) return 0;
        if (ref != nullptr) {
            ref->name = info.name;
            ref->offset = offset;
        }
        return Intern(info);
    }

    const std::vector<SymbolRecord>& records() const { return m_records; }

private:
    std::vector<SymbolRecord> m_records;
};

/// 堆对象收集器：同一对象只登记一次；类型必须有序列化工厂。
class HandleCollector {
public:
    std::uint32_t Intern(const MigratableObject& object, std::string* error) {
        const auto existing = m_ids.find(object.address);
        if (existing != m_ids.end()) return existing->second;
        const SerializerEntry* serializer = FindSerializer(object.type);
        if (serializer == nullptr) {
            std::ostringstream os;
            os << "堆对象类型「" << object.type << "」（地址 0x" << std::hex
               << object.address << std::dec
               << "）未注册序列化函数：请用 MIGRATION_SERIALIZABLE 声明 "
                  "MigrationSerialize/MigrationDeserialize";
            *error = os.str();
            return 0;
        }
        HandleRecord record{};
        record.id = static_cast<std::uint32_t>(m_records.size()) + 1;
        record.old_address = object.address;
        record.object_size = object.size;
        record.type = object.type;
        if (!serializer->serialize(reinterpret_cast<const void*>(object.address),
                                   &record.payload)) {
            *error = "堆对象类型「" + object.type + "」序列化失败";
            return 0;
        }
        const std::uint32_t id = record.id;
        m_records.push_back(std::move(record));
        m_ids[object.address] = id;
        return id;
    }

    const std::vector<HandleRecord>& records() const { return m_records; }

private:
    std::vector<HandleRecord> m_records;
    std::map<std::uint64_t, std::uint32_t> m_ids;
};

bool BuildSnapshot(FiberCtl* fiber, ChannelCtl* channel, const DumpResult& dump,
                   Snapshot* snapshot, std::string* error) {
    RefreshSafepoints();
    const std::uint64_t base = dump.old_base;
    const std::uint64_t top = dump.old_top;
    const std::uint64_t ctx = dump.ctx;
    const auto maps = ReadSelfMaps();

    // 0) 符号唯一性：同名不同址会让「按名字恢复」不再唯一，直接拒绝。
    {
        const auto conflicts = SymbolTable::Instance().Conflicts();
        if (!conflicts.empty()) {
            std::ostringstream os;
            os << "符号名冲突：同名符号注册到了不同地址（inline 克隆 / 重复注册），"
                  "按名字恢复不再唯一，共 "
               << conflicts.size() << " 处：";
            for (std::size_t i = 0; i < conflicts.size(); ++i) {
                if (i != 0) os << "；";
                os << conflicts[i].name << " @0x" << std::hex
                   << conflicts[i].first_address << " / 0x"
                   << conflicts[i].second_address << std::dec;
            }
            *error = os.str();
            return false;
        }
    }

    SymbolCatalog catalog;
    HandleCollector handle_collector;
    PointerAudit audit{};
    std::vector<Relocation> relocations;
    std::vector<SafepointSlot> safepoint_slots;

    // 1) 挂起点：id 必须在当前镜像注册，槽位必须在栈内且可符号化。
    if (fiber->safepoint_id != 0) {
        const SafepointInfo* anchor =
            SafepointTable::Instance().Find(fiber->safepoint_id);
        if (anchor == nullptr) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          "挂起点 id=0x%x 未在当前镜像的 migration_safepoints 段中注册"
                          "（MIGRATION_SAFEPOINT 未生效？）",
                          fiber->safepoint_id);
            *error = buffer;
            return false;
        }
        if (fiber->safepoint_slot < base || fiber->safepoint_slot + 8 > top) {
            *error = "挂起点返回地址槽不在 Fiber 栈内（__builtin_frame_address 记录失败？）";
            return false;
        }
        std::uint64_t return_address = 0;
        std::memcpy(&return_address,
                    reinterpret_cast<const void*>(fiber->safepoint_slot),
                    sizeof(return_address));
        SymbolInfo return_info;
        std::int64_t return_offset = 0;
        if (!SymbolTable::Instance().ResolveSymbol(return_address, &return_info,
                                                   &return_offset)) {
            *error = SymbolTable::Instance().DiagnoseUnresolved(return_address) +
                     "（挂起点的返回地址槽）";
            return false;
        }
        SymbolInfo anchor_info;
        std::int64_t anchor_offset = 0;
        if (SymbolTable::Instance().ResolveSymbol(
                reinterpret_cast<std::uint64_t>(anchor->address), &anchor_info,
                &anchor_offset)) {
            if (anchor_info.name != return_info.name) {
                std::ostringstream os;
                os << "挂起点 id=0x" << std::hex << fiber->safepoint_id
                   << " 的锚点位于 " << anchor_info.name << "，但返回地址属于 "
                   << return_info.name << "：MIGRATION_SAFEPOINT 宏可能放错了函数";
                *error = os.str();
                return false;
            }
        }
        SafepointSlot slot{};
        slot.id = fiber->safepoint_id;
        slot.symbol_id = catalog.Intern(return_info);
        slot.slot_offset = fiber->safepoint_slot - base;
        slot.old_offset = return_offset;
        safepoint_slots.push_back(slot);
        (void)anchor_offset;
    }

    // 2) 扫描栈：生成重定位并统计指针类别。
    for (std::uint64_t slot = ctx; slot + 8 <= top; slot += 8) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(slot), sizeof(value));
        ++audit.slots;
        if (value == 0) continue;
        if (fiber->safepoint_id != 0 && slot == fiber->safepoint_slot) {
            Relocation rel{};
            rel.kind = kRelocSafepoint;
            rel.target_id = fiber->safepoint_id;
            rel.slot_offset = slot - base;
            rel.addend = 0;
            relocations.push_back(rel);
            ++audit.symbol_ptrs;
            ++audit.code_ptrs;
            continue;
        }
        if (value >= base && value < top) {
            Relocation rel{};
            rel.kind = kRelocStack;
            rel.slot_offset = slot - base;
            rel.addend = static_cast<std::int64_t>(value - base);
            relocations.push_back(rel);
            ++audit.stack_ptrs;
            continue;
        }
        SymbolInfo info;
        std::int64_t offset = 0;
        if (SymbolTable::Instance().ResolveSymbol(value, &info, &offset)) {
            Relocation rel{};
            rel.kind = kRelocSymbol;
            rel.target_id = catalog.Intern(info);
            rel.slot_offset = slot - base;
            rel.addend = offset;
            relocations.push_back(rel);
            ++audit.symbol_ptrs;
            bool is_exec = false;
            bool is_heap = false;
            if (InMappings(maps, value, &is_exec, &is_heap) && is_exec) {
                ++audit.code_ptrs;
            }
            continue;
        }
        bool is_exec = false;
        bool is_heap = false;
        if (InMappings(maps, value, &is_exec, &is_heap)) {
            if (!is_exec) {
                std::size_t interior = 0;
                const MigratableObject* object =
                    FindMigratableObject(value, &interior);
                if (object != nullptr) {
                    std::string handle_error;
                    const std::uint32_t handle_id =
                        handle_collector.Intern(*object, &handle_error);
                    if (handle_id == 0) {
                        *error = handle_error;
                        return false;
                    }
                    Relocation rel{};
                    rel.kind = kRelocHandle;
                    rel.target_id = handle_id;
                    rel.slot_offset = slot - base;
                    rel.addend = static_cast<std::int64_t>(interior);
                    relocations.push_back(rel);
                    ++audit.handle_ptrs;
                    continue;
                }
                if (is_heap) {
                    std::ostringstream os;
                    os << "栈上存在未登记的堆指针 0x" << std::hex << value
                       << std::dec
                       << "（槽位 0x" << std::hex << (slot - base) << std::dec
                       << "）：v3 只迁移显式登记的可序列化对象，请在分配处调用 "
                          "MIGRATION_MIGRATE_OBJECT(ptr, \"TypeName\") 并确保该类型"
                          "注册了 MigrationSerialize/MigrationDeserialize";
                    *error = os.str();
                    return false;
                }
            }
            ++audit.other_ptrs;
        }
        // 不在任何映射中的值视为普通数据（如 64 KiB payload），不重定位。
    }

    // 3) 遍历 rbp 链：校验每一帧返回地址都能符号化。
    std::uint64_t rbp = 0;
    std::memcpy(&rbp, reinterpret_cast<const void*>(ctx + 40), sizeof(rbp));
    std::vector<FrameRecord> frames;
    for (int guard = 0; guard < 256; ++guard) {
        if (rbp < base || rbp + 16 > top) break;
        std::uint64_t ret = 0;
        std::uint64_t next = 0;
        std::memcpy(&ret, reinterpret_cast<const void*>(rbp + 8), sizeof(ret));
        std::memcpy(&next, reinterpret_cast<const void*>(rbp), sizeof(next));
        SymbolInfo info;
        std::int64_t offset = 0;
        if (!SymbolTable::Instance().ResolveSymbol(ret, &info, &offset)) {
            std::ostringstream os;
            os << "rbp 链第 " << frames.size() << " 帧：" 
               << SymbolTable::Instance().DiagnoseUnresolved(ret);
            *error = os.str();
            return false;
        }
        FrameRecord frame{};
        frame.rbp_offset = rbp - base;
        frame.ret_slot_offset = (rbp + 8) - base;
        frame.symbol_id = catalog.Intern(info);
        frame.ret_offset = offset;
        frames.push_back(frame);
        if (next <= rbp) break;
        rbp = next;
    }
    if (frames.empty()) {
        *error = "rbp 链为空：请确认已用 -fno-omit-frame-pointer 编译可迁移代码";
        return false;
    }

    // 4) 符号化控制块字段。
    SymbolRef fn_ref;
    const std::uint32_t fn_symbol_id = catalog.InternAddress(fiber->fn, &fn_ref);
    if (fn_symbol_id == 0) {
        *error = "Fiber 入口函数无法符号化：" +
                 SymbolTable::Instance().DiagnoseUnresolved(fiber->fn);
        return false;
    }
    SymbolRef channel_ref;
    const std::uint32_t channel_symbol_id = catalog.InternAddress(
        reinterpret_cast<std::uint64_t>(channel), &channel_ref);
    if (channel_symbol_id == 0) {
        *error = "通道控制块无法符号化（未注册 g_channel_ctl？）";
        return false;
    }

    // 5) 填充快照。
    snapshot->fiber_id = fiber->id;
    snapshot->stack_base_va = base;
    snapshot->stack_va = top;
    snapshot->stack_size = fiber->stack_size;
    snapshot->context_va = ctx;
    snapshot->stack_bytes = top - ctx;
    snapshot->arena_va = 0;
    snapshot->arena_size = 0;
    snapshot->channel_va = reinterpret_cast<std::uint64_t>(channel);
    snapshot->waiter_va = fiber->waiter;
    snapshot->wait_kind = kWaitChannelRecv;

    snapshot->stack.resize(static_cast<std::size_t>(snapshot->stack_bytes));
    std::memcpy(snapshot->stack.data(), reinterpret_cast<const void*>(ctx),
                snapshot->stack.size());
    std::uint64_t slots[7] = {};
    std::memcpy(slots, reinterpret_cast<const void*>(ctx), sizeof(slots));
    snapshot->registers.r12 = slots[0];
    snapshot->registers.r13 = slots[1];
    snapshot->registers.r14 = slots[2];
    snapshot->registers.r15 = slots[3];
    snapshot->registers.rbx = slots[4];
    snapshot->registers.rbp = slots[5];
    snapshot->registers.rip = slots[6];

    snapshot->channel.va = reinterpret_cast<std::uint64_t>(channel);
    snapshot->channel.capacity = channel->capacity;
    snapshot->channel.count = channel->count;
    snapshot->channel.closed = channel->closed;
    snapshot->channel.head_offset = channel->head != 0 ? channel->head - base : 0;
    snapshot->channel.tail_offset = channel->tail != 0 ? channel->tail - base : 0;
    for (std::size_t i = 0; i < 8; ++i) snapshot->channel.buffer[i] = channel->buffer[i];

    snapshot->fiber.ctx = ctx;
    snapshot->fiber.caller = fiber->caller;
    snapshot->fiber.stack_base = base;
    snapshot->fiber.stack_size = fiber->stack_size;
    snapshot->fiber.top = top;
    snapshot->fiber.fiber_id = fiber->id;
    snapshot->fiber.state = fiber->state;
    snapshot->fiber.fn = fiber->fn;
    snapshot->fiber.channel = fiber->channel;
    snapshot->fiber.waiter = fiber->waiter;
    snapshot->fiber.result = fiber->result;
    snapshot->fiber.arg = fiber->arg;
    snapshot->fiber.ctx_offset = ctx - base;
    snapshot->fiber.waiter_offset = fiber->waiter != 0 ? fiber->waiter - base : 0;
    snapshot->fiber.fn_symbol = fn_ref;
    snapshot->fiber.fn_symbol_id = fn_symbol_id;
    snapshot->fiber.channel_symbol = channel_ref;
    snapshot->fiber.channel_symbol_id = channel_symbol_id;
    snapshot->fiber.safepoint_id = fiber->safepoint_id;

    snapshot->audit = audit;
    snapshot->relocations = std::move(relocations);
    snapshot->frames = std::move(frames);
    snapshot->safepoint_slots = std::move(safepoint_slots);
    snapshot->handles = handle_collector.records();
    snapshot->symbols = catalog.records();
    // 符号哈希：引用集（排序后）+ 生成进程整表（清单也写进文件，便于恢复侧
    // 给出「新增/移除」明细）。
    {
        std::vector<std::string> names;
        names.reserve(snapshot->symbols.size());
        for (const auto& sym : snapshot->symbols) names.push_back(sym.name);
        snapshot->symbol_hash = SymbolTable::HashNames(names);
    }
    snapshot->symbol_manifest.clear();
    {
        for (const auto& sym : SymbolTable::Instance().Snapshot()) {
            snapshot->symbol_manifest.push_back(sym.name);
        }
    }
    snapshot->symbol_table_hash =
        SymbolTable::HashNames(snapshot->symbol_manifest);
    snapshot->symbol_epoch = kSymbolEpoch;

    snapshot->flags = kFlagCrossProcessReady | kFlagSymbolized;
    if (audit.code_ptrs != 0) snapshot->flags |= kFlagHasCodePointers;
    if (audit.stack_ptrs != 0) snapshot->flags |= kFlagHasStackPointers;
    if (audit.handle_ptrs != 0) snapshot->flags |= kFlagHasHandles;
    if (!snapshot->safepoint_slots.empty()) snapshot->flags |= kFlagHasSafepoints;

    std::ostringstream os;
    os << "符号化协程快照 v3：fiber#" << fiber->id << "，栈 " << snapshot->stack_bytes
       << " 字节，" << snapshot->symbols.size() << " 个符号、"
       << snapshot->relocations.size() << " 条重定位、" << snapshot->frames.size()
       << " 帧、" << snapshot->safepoint_slots.size() << " 个 safepoint、"
       << snapshot->handles.size() << " 个句柄；指针审计 stack=" << audit.stack_ptrs
       << " symbol=" << audit.symbol_ptrs << " code=" << audit.code_ptrs
       << " heap=" << audit.heap_ptrs << " handle=" << audit.handle_ptrs
       << " other=" << audit.other_ptrs;
    if (!snapshot->safepoint_slots.empty()) {
        os << "；挂起点 id=0x" << std::hex << snapshot->safepoint_slots.front().id
           << std::dec;
    }
    snapshot->summary = os.str();
    return true;
}

// ---------------------------------------------------------------------------
// 恢复（restore 方向）
// ---------------------------------------------------------------------------
bool ApplySnapshot(const Snapshot& snapshot, int first, int second,
                   std::string* error, std::string* trace) {
    RefreshSafepoints();
    // 1) 按名字解析符号；缺失的集中报告。
    std::vector<std::uint64_t> resolved(snapshot.symbols.size() + 1, 0);
    std::vector<std::uint64_t> sizes(snapshot.symbols.size() + 1, 0);
    std::vector<std::string> missing;
    for (const auto& symbol : snapshot.symbols) {
        std::uint64_t address = 0;
        if (symbol.id < sizes.size()) sizes[symbol.id] = symbol.size;
        if (!SymbolTable::Instance().Lookup(symbol.name, &address)) {
            missing.push_back(symbol.name);
            continue;
        }
        if (symbol.id < resolved.size()) resolved[symbol.id] = address;
    }
    if (!missing.empty()) {
        std::ostringstream os;
        os << "快照引用了当前进程无法解析的符号（共 " << missing.size() << " 个）：";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) os << ", ";
            os << missing[i];
        }
        os << "。请加载对应 SO，或用 MIGRATION_SYMBOL / MIGRATION_SYMBOL_FN 注册。";
        *error = os.str();
        return false;
    }
    // 1b) 挂起点 id 必须在当前镜像存在且唯一。
    std::map<std::uint32_t, const SafepointSlot*> slot_by_id;
    for (const auto& slot : snapshot.safepoint_slots) slot_by_id[slot.id] = &slot;
    for (const auto& entry : slot_by_id) {
        if (SafepointTable::Instance().Find(entry.first) == nullptr) {
            char buffer[320];
            std::snprintf(
                buffer, sizeof(buffer),
                "快照引用的挂起点 id=0x%x 在当前镜像中不存在：请用包含相同 "
                "MIGRATION_SAFEPOINT(0x%x) 的版本恢复（函数布局可以变化，id 必须一致）",
                entry.first, entry.first);
            *error = buffer;
            return false;
        }
        std::size_t occurrences = 0;
        for (const auto& anchor : SafepointTable::Instance().Entries()) {
            if (anchor.id == entry.first) ++occurrences;
        }
        if (occurrences > 1) {
            char buffer[320];
            std::snprintf(buffer, sizeof(buffer),
                          "挂起点 id=0x%x 在当前进程注册了 %zu 次（id 必须全局唯一）",
                          entry.first, occurrences);
            *error = buffer;
            return false;
        }
    }
    if (snapshot.fiber.safepoint_id != 0 &&
        slot_by_id.find(snapshot.fiber.safepoint_id) == slot_by_id.end()) {
        *error = "fiber 段记录了挂起点 id，但 Safepoints 段没有对应槽位";
        return false;
    }

    // 2) 符号表哈希自洽校验：重算「引用集」哈希并与头部比对。
    {
        std::vector<std::string> names;
        names.reserve(snapshot.symbols.size());
        for (const auto& symbol : snapshot.symbols) names.push_back(symbol.name);
        const std::uint64_t recomputed = SymbolTable::HashNames(names);
        if (snapshot.symbol_hash != 0 && recomputed != snapshot.symbol_hash) {
            *error = "符号表哈希不一致：符号目录与头部描述不符";
            return false;
        }
    }
    // 2b) 符号内偏移越界校验。
    auto offset_in_range = [&sizes](std::uint32_t id, std::int64_t offset) {
        if (id >= sizes.size() || sizes[id] == 0) return true;  // 大小未知，跳过
        return offset >= 0 && static_cast<std::uint64_t>(offset) < sizes[id];
    };
    if (!offset_in_range(snapshot.fiber.fn_symbol_id, snapshot.fiber.fn_symbol.offset)) {
        *error = "入口符号的偏移越界";
        return false;
    }
    if (!offset_in_range(snapshot.fiber.channel_symbol_id,
                         snapshot.fiber.channel_symbol.offset)) {
        *error = "通道符号的偏移越界";
        return false;
    }
    for (const auto& rel : snapshot.relocations) {
        if (rel.kind == kRelocSymbol && !offset_in_range(rel.target_id, rel.addend)) {
            *error = "重定位的符号偏移越界（symbol_id=" +
                     std::to_string(rel.target_id) + "）";
            return false;
        }
    }
    // 2c) 整表哈希：不阻断恢复（SO 版本差异是合法的），但把增删明细写进
    //     trace / stub，便于运维判断「是不是加载错了版本」。
    {
        std::vector<std::string> current;
        for (const auto& symbol : SymbolTable::Instance().Snapshot()) {
            current.push_back(symbol.name);
        }
        std::vector<std::string> snapshot_names = snapshot.symbol_manifest;
        std::sort(current.begin(), current.end());
        std::sort(snapshot_names.begin(), snapshot_names.end());
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::set_difference(current.begin(), current.end(), snapshot_names.begin(),
                            snapshot_names.end(), std::back_inserter(added));
        std::set_difference(snapshot_names.begin(), snapshot_names.end(), current.begin(),
                            current.end(), std::back_inserter(removed));
        if (!added.empty() || !removed.empty()) {
            std::ostringstream os;
            os << "符号表版本差异（不阻断恢复）：当前进程新增 " << added.size()
               << " 个、移除 " << removed.size() << " 个";
            if (!added.empty()) {
                os << "；新增:";
                for (const auto& name : added) os << " " << name;
            }
            if (!removed.empty()) {
                os << "；移除:";
                for (const auto& name : removed) os << " " << name;
            }
            const std::string message = os.str();
            Log("%s\n", message.c_str());
            if (trace != nullptr) *trace += message + "\n";
        }
    }

    // 3) 重建句柄对象（必须在应用重定位之前完成）。
    for (const auto& handle : snapshot.handles) {
        const SerializerEntry* serializer = FindSerializer(handle.type);
        if (serializer == nullptr) {
            std::ostringstream os;
            os << "快照包含类型「" << handle.type
               << "」的句柄，但当前进程没有该类型的反序列化工厂。已注册类型：";
            const auto types = RegisteredTypes();
            for (std::size_t i = 0; i < types.size(); ++i) {
                if (i != 0) os << ", ";
                os << types[i];
            }
            os << "。请用 MIGRATION_SERIALIZABLE 注册相同类型名。";
            *error = os.str();
            return false;
        }
        void* object = nullptr;
        if (!serializer->deserialize(handle.payload.data(), handle.payload.size(),
                                     &object) ||
            object == nullptr) {
            *error = "句柄 " + std::to_string(handle.id) + "（类型 " + handle.type +
                     "）反序列化失败";
            return false;
        }
        if (!InstallHandle(handle.id, object, handle.object_size,
                           handle.type.c_str())) {
            serializer->destroy(object);
            *error = "句柄 id 重复安装：" + std::to_string(handle.id);
            return false;
        }
    }

    // 4) 映射新栈。地址由内核决定，与生成进程不同 —— 这正是 v3 要验证的。
    void* new_stack = ::mmap(nullptr, static_cast<std::size_t>(snapshot.stack_size),
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_stack == MAP_FAILED) {
        *error = std::string("映射新栈失败: ") + std::strerror(errno);
        return false;
    }
    const auto new_base = reinterpret_cast<std::uint64_t>(new_stack);
    const std::uint64_t old_base = snapshot.stack_base_va;

    // 5) 拷贝栈快照到新栈的同一相对位置。
    if (snapshot.fiber.ctx_offset + snapshot.stack_bytes > snapshot.stack_size) {
        *error = "快照的 ctx_offset + stack_bytes 超出栈大小";
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(new_base + snapshot.fiber.ctx_offset),
                snapshot.stack.data(), snapshot.stack.size());

    // 6) 应用重定位。
    for (const auto& rel : snapshot.relocations) {
        if (rel.slot_offset + 8 > snapshot.stack_size) {
            *error = "重定位槽超出栈范围";
            return false;
        }
        std::uint64_t value = 0;
        if (rel.kind == kRelocStack) {
            value = new_base + static_cast<std::uint64_t>(rel.addend);
        } else if (rel.kind == kRelocSymbol) {
            if (rel.target_id >= resolved.size() || resolved[rel.target_id] == 0) {
                *error = "重定位引用了未解析的符号 id " +
                         std::to_string(rel.target_id);
                return false;
            }
            value = resolved[rel.target_id] + static_cast<std::uint64_t>(rel.addend);
        } else if (rel.kind == kRelocHandle) {
            std::uint64_t object_size = 0;
            void* object = LookupHandle(rel.target_id, &object_size);
            if (object == nullptr) {
                *error = "重定位引用了未重建的句柄 id " +
                         std::to_string(rel.target_id);
                return false;
            }
            if (rel.addend < 0 ||
                static_cast<std::uint64_t>(rel.addend) >= object_size) {
                *error = "句柄内偏移越界（handle_id=" +
                         std::to_string(rel.target_id) + "）";
                return false;
            }
            value = reinterpret_cast<std::uint64_t>(object) +
                    static_cast<std::uint64_t>(rel.addend);
        } else if (rel.kind == kRelocSafepoint) {
            const SafepointInfo* anchor = SafepointTable::Instance().Find(rel.target_id);
            if (anchor == nullptr) {
                *error = "重定位引用了当前镜像不存在的挂起点 id " +
                         std::to_string(rel.target_id);
                return false;
            }
            // 校验 id 没有被新版本复用给另一个函数。
            const auto slot_it = slot_by_id.find(rel.target_id);
            if (slot_it != slot_by_id.end()) {
                SymbolInfo old_info;
                std::int64_t old_offset = 0;
                const std::string* old_name = nullptr;
                for (const auto& symbol : snapshot.symbols) {
                    if (symbol.id == slot_it->second->symbol_id) {
                        old_name = &symbol.name;
                        break;
                    }
                }
                SymbolInfo new_info;
                std::int64_t new_offset = 0;
                if (old_name != nullptr &&
                    SymbolTable::Instance().ResolveSymbol(anchor->address, &new_info,
                                                          &new_offset) &&
                    new_info.name != *old_name) {
                    std::ostringstream os;
                    os << "挂起点 id=0x" << std::hex << rel.target_id
                       << " 被复用：快照里属于 " << *old_name << "，当前镜像里属于 "
                       << new_info.name;
                    *error = os.str();
                    return false;
                }
                (void)old_info;
                (void)old_offset;
            }
            value = reinterpret_cast<std::uint64_t>(anchor->address);
        } else {
            *error = "未知的重定位类型 " + std::to_string(rel.kind);
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(new_base + rel.slot_offset), &value,
                    sizeof(value));
    }

    // 7) 重建控制块：地址全部来自符号解析、safepoint 锚点与新栈基址。
    auto* const fiber = &g_fiber_ctl;
    auto* const channel = &g_channel_ctl;
    *fiber = FiberCtl{};
    *channel = ChannelCtl{};

    if (snapshot.fiber.fn_symbol_id >= resolved.size() ||
        resolved[snapshot.fiber.fn_symbol_id] == 0) {
        *error = "入口符号未解析";
        return false;
    }
    fiber->fn = resolved[snapshot.fiber.fn_symbol_id] +
                static_cast<std::uint64_t>(snapshot.fiber.fn_symbol.offset);
    if (snapshot.fiber.channel_symbol_id >= resolved.size() ||
        resolved[snapshot.fiber.channel_symbol_id] == 0) {
        *error = "通道符号未解析";
        return false;
    }
    const std::uint64_t channel_address =
        resolved[snapshot.fiber.channel_symbol_id] +
        static_cast<std::uint64_t>(snapshot.fiber.channel_symbol.offset);
    fiber->channel = channel_address;
    fiber->ctx = new_base + snapshot.fiber.ctx_offset;
    fiber->caller = 0;  // 恢复点位于 ParkSelf 内，返回时由代码自动刷新
    fiber->stack_base = new_base;
    fiber->stack_size = snapshot.stack_size;
    fiber->top = new_base + snapshot.stack_size;
    fiber->id = snapshot.fiber.fiber_id;
    fiber->state = static_cast<std::uint32_t>(FiberState::kParked);
    fiber->waiter = snapshot.fiber.waiter_offset != 0
                        ? new_base + snapshot.fiber.waiter_offset
                        : 0;
    fiber->result = snapshot.fiber.result;
    fiber->arg = snapshot.fiber.arg;
    fiber->safepoint_id = snapshot.fiber.safepoint_id;
    fiber->safepoint_slot = 0;
    if (snapshot.fiber.safepoint_id != 0) {
        const auto it = slot_by_id.find(snapshot.fiber.safepoint_id);
        if (it != slot_by_id.end()) {
            fiber->safepoint_slot = new_base + it->second->slot_offset;
        }
    }
    fiber->parked = 0;

    channel->head = snapshot.channel.head_offset != 0
                        ? new_base + snapshot.channel.head_offset
                        : 0;
    channel->tail = snapshot.channel.tail_offset != 0
                        ? new_base + snapshot.channel.tail_offset
                        : 0;
    channel->count = snapshot.channel.count;
    channel->capacity = snapshot.channel.capacity;
    channel->closed = snapshot.channel.closed;
    for (std::size_t i = 0; i < 8; ++i) channel->buffer[i] = snapshot.channel.buffer[i];

    if (channel_address != reinterpret_cast<std::uint64_t>(channel)) {
        *error = "通道符号解析到了非预期对象（期望 g_channel_ctl）";
        return false;
    }
    if (fiber->waiter != 0 &&
        (fiber->waiter < new_base || fiber->waiter >= fiber->top)) {
        *error = "等待节点不在新栈范围内";
        return false;
    }
    if (channel->head != fiber->waiter) {
        *error = "通道队列头与 Fiber 等待节点不一致";
        return false;
    }

    if (trace != nullptr) {
        std::ostringstream os;
        os << "旧栈基址 0x" << std::hex << old_base << " → 新栈基址 0x" << new_base
           << std::dec << "（内核决定，与生成进程不同）\n";
        os << "已应用 " << snapshot.relocations.size() << " 条重定位，"
           << snapshot.frames.size() << " 帧返回地址已按符号重写\n";
        *trace += os.str();
    }

    Log("旧栈 0x%llx → 新栈 0x%llx；入口 %s+%lld → 0x%llx\n",
        static_cast<unsigned long long>(old_base),
        static_cast<unsigned long long>(new_base),
        snapshot.fiber.fn_symbol.name.c_str(),
        static_cast<long long>(snapshot.fiber.fn_symbol.offset),
        static_cast<unsigned long long>(fiber->fn));

    // 挂起点证据：旧偏移来自生成镜像，新偏移来自当前镜像 —— 布局变化不影响。
    for (const auto& slot : snapshot.safepoint_slots) {
        const SafepointInfo* anchor = SafepointTable::Instance().Find(slot.id);
        std::string old_name = "?";
        for (const auto& symbol : snapshot.symbols) {
            if (symbol.id == slot.symbol_id) {
                old_name = symbol.name;
                break;
            }
        }
        SymbolInfo new_info;
        std::int64_t new_offset = 0;
        std::string new_name = anchor != nullptr ? anchor->function : "缺失";
        if (anchor != nullptr &&
            SymbolTable::Instance().ResolveSymbol(anchor->address, &new_info,
                                                  &new_offset)) {
            new_name = new_info.name;
        }
        Log("挂起点 id=0x%x：旧 %s+0x%llx → 新 %s+0x%llx（布局变化不影响）\n",
            slot.id, old_name.c_str(),
            static_cast<unsigned long long>(slot.old_offset), new_name.c_str(),
            static_cast<unsigned long long>(new_offset));
        if (trace != nullptr) {
            std::ostringstream os;
            os << "safepoint 0x" << std::hex << slot.id << std::dec << ": " << old_name
               << "+0x" << std::hex
               << static_cast<unsigned long long>(slot.old_offset) << std::dec
               << " → " << new_name << "+0x" << std::hex
               << static_cast<unsigned long long>(new_offset) << std::dec << "\n";
            *trace += os.str();
        }
    }

    Log("发送第 1 条消息 %d\n", first);
    if (!ChannelSend(channel, first, error)) {
        return false;
    }
    Log("第 1 条消息已送达，Fiber 再次挂起于 state=%u（2=Parked）\n", fiber->state);
    Log("发送第 2 条消息 %d\n", second);
    if (!ChannelSend(channel, second, error)) {
        return false;
    }
    if (fiber->state != static_cast<std::uint32_t>(FiberState::kCompleted)) {
        *error = "Fiber 未在两条消息后完成（state=" +
                 std::to_string(fiber->state) + "）";
        return false;
    }
    if (fiber->result != second) {
        *error = "Fiber 收到的最后一个值不是预期消息（" +
                 std::to_string(fiber->result) + " != " + std::to_string(second) + "）";
        return false;
    }
    Log("Fiber 已完成，result=%lld（期望 %d）\n",
        static_cast<long long>(fiber->result), second);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
std::int64_t ChannelRecv(ChannelCtl* channel, FiberCtl* self) {
    return WaitForValue(channel, self);
}

__attribute__((noinline)) std::int64_t ChannelRecvAt(ChannelCtl* channel,
                                                     FiberCtl* self,
                                                     std::uint32_t safepoint_id) {
    if (self != nullptr) {
        self->safepoint_id = safepoint_id;
        // 本函数自己的返回地址槽 = frame_address(0) + 8（x86-64 帧布局）。
        // 恢复时把它改写成当前镜像的 safepoint 锚点，ChannelRecvAt 的 ret
        // 就会落到用户函数里「调用之后」的语义位置。
        const void* frame = __builtin_frame_address(0);
        self->safepoint_slot =
            frame != nullptr ? reinterpret_cast<std::uint64_t>(frame) + 8 : 0;
    }
    const std::int64_t value = WaitForValue(channel, self);
    if (self != nullptr) {
        self->safepoint_id = 0;
        self->safepoint_slot = 0;
    }
    return value;
}

__attribute__((noinline)) void ParkAt(FiberCtl* self, std::uint32_t safepoint_id) {
    if (self == nullptr) return;
    self->safepoint_id = safepoint_id;
    const void* frame = __builtin_frame_address(0);
    self->safepoint_slot =
        frame != nullptr ? reinterpret_cast<std::uint64_t>(frame) + 8 : 0;
    self->parked = 1;
    self->state = static_cast<std::uint32_t>(FiberState::kParked);
    while (self->parked != 0) {
        ParkSelf(self);
    }
    self->state = static_cast<std::uint32_t>(FiberState::kRunning);
    self->safepoint_id = 0;
    self->safepoint_slot = 0;
}

bool ResumeParkedFiber(FiberCtl* self, std::int64_t value) {
    if (self == nullptr || self->parked == 0) return false;
    ArmSafepointResume(self->safepoint_id, value);
    self->parked = 0;
    ResumeFiber(self);
    return true;
}

void SetProcessRole(const char* role) {
    g_role = (role != nullptr && role[0] != '\0') ? role : "?";
}

bool LoadDemoLibrary(const std::string& path, std::string* error) {
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        const char* reason = ::dlerror();
        *error = std::string("dlopen 失败: ") + (reason != nullptr ? reason : "unknown");
        return false;
    }
    void* symbol = ::dlsym(handle, "migration_demo_body");
    if (symbol == nullptr) {
        *error = "SO 中缺少导出符号 migration_demo_body";
        return false;
    }
    g_demo_body = reinterpret_cast<DemoBodyFn>(symbol);
    // 新模块的 migration_safepoints 段是加载后才出现在内存里的：必须刷新。
    RefreshSafepoints();
    return true;
}

bool DumpDemoToFile(const std::string& path, std::string* error) {
    RefreshSafepoints();
    void* stack = ::mmap(nullptr, static_cast<std::size_t>(kDefaultStackSize),
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        *error = std::string("映射 Fiber 栈失败: ") + std::strerror(errno);
        return false;
    }
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(stack);
    const std::uint64_t top = base + kDefaultStackSize;

    auto* const fiber = &g_fiber_ctl;
    auto* const channel = &g_channel_ctl;
    *fiber = FiberCtl{};
    *channel = ChannelCtl{};

    fiber->stack_base = base;
    fiber->stack_size = kDefaultStackSize;
    fiber->top = top;
    fiber->id = 1;
    fiber->fn = reinterpret_cast<std::uint64_t>(ActiveDemoBody());
    fiber->channel = reinterpret_cast<std::uint64_t>(channel);
    fiber->state = static_cast<std::uint32_t>(FiberState::kReady);
    fiber->ctx = reinterpret_cast<std::uint64_t>(
        MakeContext(stack, static_cast<std::size_t>(kDefaultStackSize), &FiberEntry));
    if (fiber->ctx == 0) {
        *error = "MakeContext 失败";
        return false;
    }
    Log("栈=[0x%llx,0x%llx) 初始 ctx=0x%llx 入口=%p\n",
        static_cast<unsigned long long>(base),
        static_cast<unsigned long long>(top),
        static_cast<unsigned long long>(fiber->ctx),
        reinterpret_cast<void*>(fiber->fn));

    ResumeFiber(fiber);
    if (fiber->state != static_cast<std::uint32_t>(FiberState::kParked)) {
        *error = "Fiber 未按预期挂起（state=" + std::to_string(fiber->state) + "）";
        return false;
    }
    if (fiber->waiter == 0 || channel->head != fiber->waiter) {
        *error = "等待节点未按预期登记到通道队列";
        return false;
    }
    Log("Fiber 已挂起：ctx=0x%llx waiter=0x%llx channel.head=0x%llx safepoint=0x%x\n",
        static_cast<unsigned long long>(fiber->ctx),
        static_cast<unsigned long long>(fiber->waiter),
        static_cast<unsigned long long>(channel->head), fiber->safepoint_id);

    Snapshot snapshot{};
    DumpResult dump{};
    dump.old_base = base;
    dump.old_top = top;
    dump.ctx = fiber->ctx;
    if (!BuildSnapshot(fiber, channel, dump, &snapshot, error)) {
        return false;
    }
    snapshot.build_id_hash = SelfBuildIdHash();

    Log("栈快照 %llu 字节，符号 %zu 个，重定位 %zu 条，帧 %zu 个，safepoint %zu 个，"
        "句柄 %zu 个\n",
        static_cast<unsigned long long>(snapshot.stack_bytes), snapshot.symbols.size(),
        snapshot.relocations.size(), snapshot.frames.size(),
        snapshot.safepoint_slots.size(), snapshot.handles.size());
    Log("指针审计 stack=%u symbol=%u code=%u heap=%u handle=%u other=%u slots=%u\n",
        snapshot.audit.stack_ptrs, snapshot.audit.symbol_ptrs,
        snapshot.audit.code_ptrs, snapshot.audit.heap_ptrs,
        snapshot.audit.handle_ptrs, snapshot.audit.other_ptrs,
        snapshot.audit.slots);

    if (!WriteSnapshotFile(path, snapshot, error)) {
        return false;
    }
    Log("已写出快照：%s（%llu 字节栈）\n", path.c_str(),
        static_cast<unsigned long long>(snapshot.stack_bytes));
    return true;
}

bool RestoreDemoFromFile(const std::string& path, int first, int second,
                         std::string* error, std::string* trace) {
    Snapshot snapshot{};
    if (!ReadSnapshotFile(path, &snapshot, error)) {
        return false;
    }
    {
        std::ostringstream os;
        os << "快照校验通过（magic/version/CRC/签名/build-id/symbols 全部匹配）：\n"
           << DescribeSnapshot(snapshot);
        if (trace != nullptr) *trace += os.str();
        std::printf("%s", os.str().c_str());
        std::fflush(stdout);
    }
    const bool ok = ApplySnapshot(snapshot, first, second, error, trace);
    // 恢复出的 Fiber 已经跑完（或失败），重建对象可以安全析构。
    ReleaseHandles();
    return ok;
}

bool VerifySnapshotFile(const std::string& path, std::string* error,
                        std::string* report) {
    Snapshot snapshot{};
    if (!ReadSnapshotFile(path, &snapshot, error)) {
        return false;
    }
    std::string text = DescribeSnapshot(snapshot);
    std::vector<std::string> missing;
    for (const auto& symbol : snapshot.symbols) {
        std::uint64_t address = 0;
        if (!SymbolTable::Instance().Lookup(symbol.name, &address)) {
            missing.push_back(symbol.name);
        }
    }
    if (!missing.empty()) {
        text += "缺失符号: ";
        for (const auto& name : missing) text += name + " ";
        text += "\n";
    }
    // 挂起点可用性
    for (const auto& slot : snapshot.safepoint_slots) {
        if (SafepointTable::Instance().Find(slot.id) == nullptr) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          "缺失挂起点: id=0x%x（当前进程未注册）\n", slot.id);
            text += buffer;
        }
    }
    // 符号表版本差异明细
    {
        std::vector<std::string> current;
        for (const auto& symbol : SymbolTable::Instance().Snapshot()) {
            current.push_back(symbol.name);
        }
        std::vector<std::string> snapshot_names = snapshot.symbol_manifest;
        std::sort(current.begin(), current.end());
        std::sort(snapshot_names.begin(), snapshot_names.end());
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::set_difference(current.begin(), current.end(), snapshot_names.begin(),
                            snapshot_names.end(), std::back_inserter(added));
        std::set_difference(snapshot_names.begin(), snapshot_names.end(), current.begin(),
                            current.end(), std::back_inserter(removed));
        const bool hash_match =
            SymbolTable::HashNames(current) ==
            SymbolTable::HashNames(snapshot_names);
        text += hash_match ? "符号表: 与快照一致\n" : "符号表: 与快照不一致\n";
        if (!added.empty()) {
            text += "  当前新增:";
            for (const auto& name : added) text += " " + name;
            text += "\n";
        }
        if (!removed.empty()) {
            text += "  当前缺失:";
            for (const auto& name : removed) text += " " + name;
            text += "\n";
        }
    }
    // 当前进程的符号冲突
    const auto conflicts = SymbolTable::Instance().Conflicts();
    if (!conflicts.empty()) {
        text += "当前进程符号冲突:\n";
        for (const auto& conflict : conflicts) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "  %s @0x%llx / 0x%llx\n",
                          conflict.name.c_str(),
                          static_cast<unsigned long long>(conflict.first_address),
                          static_cast<unsigned long long>(conflict.second_address));
            text += buffer;
        }
    }
    if (report != nullptr) *report = text;
    return true;
}

}  // namespace migration
