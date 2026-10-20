/**
 * @file format.hpp
 * @brief 「可迁移协程」快照文件的稳定二进制格式（v3：safepoint id + 句柄 + 签名）。
 *
 * ## v3 与 v2 的差别
 *
 * | 维度 | v2 | v3 |
 * |------|----|----|
 * | 用户函数内挂起点 | `{符号, 指令偏移}` | **`{符号, safepoint id}`** |
 * | 栈上堆指针 | 一律拒绝 | **已登记的显式可序列化对象 → 句柄** |
 * | 完整性 | CRC32 | CRC32 + **HMAC-SHA256 签名**（默认强制） |
 * | 符号名 | 源码名 | 源码名 + **demangled 显示名**（lambda 可读） |
 * | 符号版本 | 引用集哈希 | 引用集哈希 + **完整表哈希 + epoch** |
 *
 * ## 设计目标
 *
 * - **版本化**：magic + version + header_size，旧读端可拒绝新文件；
 * - **自校验**：头部与每个段都有 CRC32，整文件另有 CRC32 与 HMAC；
 * - **跨进程可读**：显式小端、固定宽度、手工写字节，不依赖 padding；
 * - **可拒绝**：build-id、签名、符号缺失/冲突、id 缺失、offset 越界都明确失败；
 * - **布局无关**：用户函数内的挂起点用 safepoint id 表达，函数布局变化不影响。
 *
 * ## 文件布局
 *
 * ```
 * [0,224)              Header
 * [224,224+48*N)       Section table（每项 48 字节）
 * 8 字节对齐后          各 Section 数据，顺序与段表一致
 * 末尾（通常是最后一节）  Signature 段（64 字节，HMAC-SHA256）
 * ```
 *
 * ## Header 字段（小端，单位字节）
 *
 * ```
 *   0  char[8] magic          "G2CMIG03"
 *   8  u32     version        3
 *  12  u32     header_size    224
 *  16  u32     flags          Flag 位集合
 *  20  u32     section_count
 *  24  u16     arch           62 = EM_X86_64
 *  26  u8      pointer_size   8
 *  27  u8      endianness     0 = little
 *  28  u64     build_id_hash  生成者 /proc/self/exe 的 GNU build-id 哈希
 *  36  u64     file_size
 *  44  u64     fiber_id
 *  52  u64     stack_va       旧栈顶（诊断用）
 *  60  u64     stack_size     旧栈映射大小
 *  68  u64     context_va     旧上下文指针（诊断用）
 *  76  u64     stack_bytes    快照字节数
 *  84  u64     arena_va       旧 arena / 全局对象地址（诊断用）
 *  92  u64     arena_size
 * 100  u64     channel_va     旧通道控制块地址（诊断用）
 * 108  u64     waiter_va      旧等待节点地址（诊断用）
 * 116  u32     wait_kind      0=none 1=recv 2=send
 * 120  u32     crc32          整文件 CRC32；计算时本字段视为 0
 * 124  u32     symbol_epoch   符号系统版本（v3 = 1），不匹配即拒绝
 * 128  u64     stack_base_va  旧栈基址（重定位的参考原点）
 * 136  u64     symbol_hash    快照引用符号名集合哈希（跨版本稳定）
 * 144  u32     symbol_count   符号目录条目数
 * 148  u32     reloc_count    重定位条目数
 * 152  u32     frame_count    帧链条目数
 * 156  u32     safepoint_count 挂起点槽位数
 * 160  u64     ctx_offset     ctx - stack_base
 * 168  u64     waiter_offset  waiter - stack_base
 * 176  u64     symbol_table_hash 生成进程完整符号表哈希（诊断/版本比对）
 * 184  u64     signature_offset 签名段偏移（0 = 未签名）
 * 192  u32     handle_count   句柄数
 * 196  u32     safepoint_schema 挂起点段版本（1）
 * 200  u64     signature_size 签名段长度（64）
 * 208  u64     reserved5
 * 216  u64     reserved6
 * ```
 *
 * ## 段类型
 *
 * | type | 名称 | 内容 |
 * |------|------|------|
 * | 1 | Stack | 挂起栈原始字节 `[context_va, stack_va)` |
 * | 2 | Registers | 保存区 7 个 u64：r12 r13 r14 r15 rbx rbp rip |
 * | 3 | Channel | 等待通道状态（含 head/tail 的栈内偏移） |
 * | 4 | PointerAudit | 栈上指针分类计数（不可迁移性证据） |
 * | 5 | Summary | UTF-8 文本摘要 |
 * | 6 | FiberState | Fiber 控制块权威状态（含符号引用与 safepoint id） |
 * | 7 | Symbols | 符号目录：`{id, kind, size, name, display}`，**不含地址** |
 * | 8 | Relocations | 栈内重定位：`{kind, target_id, slot_offset, addend}` |
 * | 9 | Frames | rbp 帧链诊断：`{rbp_offset, ret_slot_offset, symbol_id, ret_offset}` |
 * | 10 | Safepoints | 挂起点槽位：`{id, symbol_id, slot_offset, old_offset}` |
 * | 11 | Handles | 句柄表：`{id, old_address, object_size, type, payload}` |
 * | 12 | Signature | 签名：`{magic, alg, flags, key_id[16], mac[32]}` |
 * | 13 | SymbolManifest | 生成进程完整符号表名字清单（版本差异诊断） |
 *
 * 恢复必需：Stack + Registers + FiberState + Symbols + Relocations + Signature；
 * 有等待时还需要 Channel；Safepoints/Handles 在需要时出现。
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace migration {

/// 快照魔数。v3 与 v2 不兼容，读端按 magic/version 拒绝。
inline constexpr char kMagic[8] = {'G', '2', 'C', 'M', 'I', 'G', '0', '3'};
/// 当前格式版本。
inline constexpr std::uint32_t kFormatVersion = 3;
/// Header 固定长度（字节）。
inline constexpr std::uint32_t kHeaderSize = 224;
/// 段表每项长度（字节）。
inline constexpr std::uint32_t kSectionEntrySize = 48;
/// x86-64 的 ELF machine 值。
inline constexpr std::uint16_t kArchX86_64 = 62;
/// 符号系统版本；符号命名/注册语义变化时递增。
inline constexpr std::uint32_t kSymbolEpoch = 1;
/// 挂起点段格式版本。
inline constexpr std::uint32_t kSafepointSchema = 1;

/// 头部标志位。
enum HeaderFlags : std::uint32_t {
    /// 恢复所需信息齐全，允许跨进程恢复。
    kFlagCrossProcessReady = 1U << 0,
    /// 栈上存在**未句柄化**的堆指针（v3 正常不会出现，出现即拒绝）。
    kFlagHasHeapPointers = 1U << 1,
    /// 栈上存在代码地址，要求符号可解析。
    kFlagHasCodePointers = 1U << 2,
    /// 栈上存在指向栈自身的指针，恢复时按旧基址平移。
    kFlagHasStackPointers = 1U << 3,
    /// 生成者依赖 ASLR 布局（v2 起恒为 0，保留位）。
    kFlagAslrDependent = 1U << 4,
    /// 快照中的地址已经全部符号化（v2 起恒置位）。
    kFlagSymbolized = 1U << 5,
    /// 句柄表中存在重建对象。
    kFlagHasHandles = 1U << 6,
    /// 快照带 HMAC-SHA256 签名（v3 正常恒置位）。
    kFlagSigned = 1U << 7,
    /// 用户函数内存在 safepoint 槽位。
    kFlagHasSafepoints = 1U << 8,
};

/// 段类型。
enum SectionType : std::uint32_t {
    kSectionStack = 1,
    kSectionRegisters = 2,
    kSectionChannel = 3,
    kSectionPointerAudit = 4,
    kSectionSummary = 5,
    kSectionFiberState = 6,
    kSectionSymbols = 7,
    kSectionRelocations = 8,
    kSectionFrames = 9,
    kSectionSafepoints = 10,
    kSectionHandles = 11,
    kSectionSignature = 12,
    kSectionSymbolManifest = 13,
};

/// 等待类型。
enum WaitKind : std::uint32_t {
    kWaitNone = 0,
    kWaitChannelRecv = 1,
    kWaitChannelSend = 2,
};

/// 重定位种类。
enum RelocationKind : std::uint32_t {
    /// 指向旧栈内部：新地址 = 新栈基址 + addend（旧栈内偏移）。
    kRelocStack = 1,
    /// 指向符号：新地址 = Lookup(symbol) + addend。
    kRelocSymbol = 2,
    /// 指向重建的堆对象：新地址 = Handle(target_id) + addend（对象内偏移）。
    kRelocHandle = 3,
    /// 指向 safepoint 锚点：新地址 = Safepoint(target_id).address，addend 必须为 0。
    kRelocSafepoint = 4,
};

/// 符号种类。
enum SymbolKind : std::uint32_t {
    kSymbolUnknown = 0,
    kSymbolFunction = 1,
    kSymbolData = 2,
    kSymbolLambda = 3,
};

/// 一个符号引用（`{name, offset}` 的逻辑视图）。
struct SymbolRef {
    std::string name;
    std::int64_t offset{0};
};

/// 符号目录条目。地址不入文件，恢复侧按名字解析。
struct SymbolRecord {
    std::uint32_t id{0};
    std::uint32_t kind{0};  ///< SymbolKind
    std::uint64_t size{0};
    std::string name;       ///< 稳定键（注册名 / demangled 名）
    std::string display;    ///< 可读名（多为 demangled），可为空
};

/// 一条栈内重定位。
struct Relocation {
    std::uint32_t kind{0};        ///< RelocationKind
    std::uint32_t target_id{0};   ///< 符号 id / 句柄 id / safepoint id
    std::uint64_t slot_offset{0}; ///< 槽位相对旧栈基址的偏移
    std::int64_t addend{0};
};

/// 帧链的一帧（诊断用；恢复不依赖，但帧链断裂会在 dump 侧拒绝）。
struct FrameRecord {
    std::uint64_t rbp_offset{0};
    std::uint64_t ret_slot_offset{0};
    std::uint32_t symbol_id{0};
    std::int64_t ret_offset{0};
};

/// 一个用户函数内的挂起点槽位（返回地址槽）。
struct SafepointSlot {
    std::uint32_t id{0};
    std::uint32_t symbol_id{0};    ///< 挂起点所在函数的符号 id（诊断/校验）
    std::uint64_t slot_offset{0};  ///< 返回地址槽相对旧栈基址
    std::int64_t old_offset{0};    ///< 旧镜像中的指令偏移（诊断）
};

/// 一个可迁移堆对象的句柄。
struct HandleRecord {
    std::uint32_t id{0};
    std::uint32_t reserved{0};
    std::uint64_t old_address{0};   ///< 旧地址（诊断）
    std::uint64_t object_size{0};   ///< 对象字节数（内部偏移上界）
    std::string type;               ///< 类型名（对应 SerializerEntry::type）
    std::vector<std::uint8_t> payload;
};

/// 签名状态（读入后由校验填写 `verified`）。
struct SignatureState {
    bool present{false};
    bool verified{false};
    std::uint32_t algorithm{0};
    std::uint32_t flags{0};
    std::array<std::uint8_t, 16> key_id{};
    std::array<std::uint8_t, 32> mac{};
};

/// 通道的持久状态。head/tail 一定指向栈内等待节点，因此用偏移表达。
struct ChannelState {
    std::uint64_t va{0};          ///< 旧地址（诊断）
    std::uint64_t capacity{0};
    std::uint64_t count{0};
    std::uint64_t head_offset{0}; ///< head - stack_base；无等待者时 0
    std::uint64_t tail_offset{0};
    std::uint32_t closed{0};
    std::uint32_t reserved{0};
    std::array<std::int64_t, 8> buffer{};
};

/// 栈上指针的分类计数。
struct PointerAudit {
    std::uint32_t stack_ptrs{0};
    std::uint32_t symbol_ptrs{0};
    std::uint32_t code_ptrs{0};
    std::uint32_t heap_ptrs{0};    ///< 未登记、无法迁移的堆指针（0 = 可恢复）
    std::uint32_t handle_ptrs{0};  ///< 已转成句柄的堆指针
    std::uint32_t other_ptrs{0};
    std::uint32_t slots{0};
};

/// 保存区中 7 个 callee-saved / 控制寄存器的逻辑视图。
struct SavedRegisters {
    std::uint64_t r12{0};
    std::uint64_t r13{0};
    std::uint64_t r14{0};
    std::uint64_t r15{0};
    std::uint64_t rbx{0};
    std::uint64_t rbp{0};
    std::uint64_t rip{0};
};

/// Fiber 控制块的权威状态。地址字段仅作诊断；恢复按符号/偏移重建。
struct FiberStateSnapshot {
    std::uint64_t ctx{0};         ///< 旧 ctx（诊断）
    std::uint64_t caller{0};      ///< 旧 caller（诊断，不恢复）
    std::uint64_t stack_base{0};  ///< 旧栈基址
    std::uint64_t stack_size{0};
    std::uint64_t top{0};
    std::uint64_t fiber_id{0};
    std::uint32_t state{0};       ///< 0=Ready 1=Running 2=Parked 3=Completed 4=Failed
    std::uint32_t reserved{0};
    std::uint64_t fn{0};          ///< 旧入口地址（诊断）
    std::uint64_t channel{0};     ///< 旧通道地址（诊断）
    std::uint64_t waiter{0};      ///< 旧等待节点地址（诊断）
    std::int64_t result{0};
    std::uint64_t arg{0};
    // ---- v2 符号化字段 ----
    std::uint64_t ctx_offset{0};     ///< ctx - stack_base
    std::uint64_t waiter_offset{0};  ///< waiter - stack_base；无等待为 0
    SymbolRef fn_symbol;             ///< 入口函数
    SymbolRef channel_symbol;        ///< 通道控制块（全局对象）
    std::uint32_t fn_symbol_id{0};
    std::uint32_t channel_symbol_id{0};
    // ---- v3 挂起点字段 ----
    std::uint32_t safepoint_id{0};    ///< 挂起时所在的 safepoint id（0 = 无）
    std::uint32_t safepoint_reserved{0};
};

/// 一份完整的快照（逻辑视图，读写两端共用）。
struct Snapshot {
    std::uint32_t flags{0};
    std::uint32_t symbol_epoch{kSymbolEpoch};
    std::uint64_t build_id_hash{0};
    std::uint64_t symbol_hash{0};
    std::uint64_t symbol_table_hash{0};
    std::uint64_t fiber_id{0};
    std::uint64_t stack_va{0};
    std::uint64_t stack_size{0};
    std::uint64_t stack_base_va{0};
    std::uint64_t context_va{0};
    std::uint64_t stack_bytes{0};
    std::uint64_t arena_va{0};
    std::uint64_t arena_size{0};
    std::uint64_t channel_va{0};
    std::uint64_t waiter_va{0};
    std::uint32_t wait_kind{kWaitNone};
    std::vector<std::uint8_t> stack;
    SavedRegisters registers{};
    ChannelState channel{};
    FiberStateSnapshot fiber{};
    PointerAudit audit{};
    std::vector<SymbolRecord> symbols;
    std::vector<Relocation> relocations;
    std::vector<FrameRecord> frames;
    std::vector<SafepointSlot> safepoint_slots;
    std::vector<HandleRecord> handles;
    std::vector<std::string> symbol_manifest;  ///< 生成进程完整符号表名字清单
    SignatureState signature;
    std::string summary;
};

/// 计算 CRC32（IEEE 802.3 多项式 0xEDB88320，与 zlib 一致）。
std::uint32_t Crc32(const void* data, std::size_t size, std::uint32_t seed = 0);

/// 读取本进程可执行文件的 GNU build-id 并哈希成 u64。
std::uint64_t SelfBuildIdHash();

/**
 * @brief 把快照写为稳定格式文件（带签名）。
 *
 * 签名策略：
 * - 设置了 `MIGRATION_SIGNING_KEY[_FILE]` → HMAC-SHA256；
 * - 都没有且未设 `MIGRATION_INSECURE_NO_SIGNATURE=1` → 失败（拒绝生成未签名快照）；
 * - 显式 `MIGRATION_INSECURE_NO_SIGNATURE=1` → 生成未签名快照（仅测试）。
 */
bool WriteSnapshotFile(const std::string& path, const Snapshot& snapshot,
                       std::string* error);

/// 从稳定格式文件读回快照，并做 magic/version/长度/CRC/签名/结构校验。
bool ReadSnapshotFile(const std::string& path, Snapshot* out, std::string* error);

/// 供 CLI 使用的可读校验摘要。
std::string DescribeSnapshot(const Snapshot& snapshot);

}  // namespace migration
