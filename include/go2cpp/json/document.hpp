#pragma once

/**
 * @file document.hpp
 * @brief JSON 解析结果的扁平池容器：`bgo2cpp::json::Document`。
 *
 * ## 结构（P6b tape-lite）
 * ```
 * Document
 * ├── m_inline_arena[4096] / m_arena   // 一次分配：节点 / 成员 / 两级 scratch / 数组索引
 * │   ├── detail::Node   nodes         // 节点池：每值 32 B 定长槽
 * │   ├── Member         members       // 对象成员池：直接写入的成员按解析顺序追加
 * │   ├── Member         member_scratch// 成员临时区：仅被插入打断的容器使用
 * │   ├── uint32_t       elements      // 数组元素索引池
 * │   └── uint32_t       element_scratch
 * ├── std::vector<char>  strings       // 转义解码后的字符串字节（按需，首次转义才分配）
 * ├── std::vector<Decimal> decimals   // 超出 int64/double 的数值（按需）
 * ├── std::vector<ErrorPtr> errors    // 坏值错误清单（按需，仅 kSkipValue/kCollect）
 * └── Value               root         // 根句柄（{this, 0} 或 null）
 * ```
 *
 * 五个热池共用一块 arena：**≤4 KiB 的输入（典型配置/API 响应）零堆分配**，
 * 超出时才 `new char[]`；深嵌套触发 `GrowPools`（2× 扩容 + memcpy，句柄全是
 * 下标所以搬迁安全）。
 *
 * ## 为什么数组要用独立的索引池（对设计文档的修正）
 * 设计稿写「数组的 `Span` 指向 `values_` 里连续的 `Value`」。但单遍深度优先
 * 解析下，数组元素头节点并不连续：元素 A 的**子树**会插在 A 与下一个元素 B
 * 之间。若坚持「连续」，`At(i)` 就必须按每棵子树的大小逐跳累加（O(i)），或者
 * 给每个节点再加一个「子树尾」字段。这里改成**数组元素索引池**：
 *
 * - 数组节点存 `Span{begin, count}`，指向 `elements_` 的连续区间；
 * - `elements_[k]` 是第 k 个元素的头节点下标；
 * - `At(i)` 是 O(1) 的两跳查表，代价是每元素 4 B。
 *
 * 对象成员天然连续（`members_` 按提交顺序追加），`At(i)` / `MemberName(i)`
 * 直接按下标取。两者共用 `Span`，由 `NodeKind` 区分语义。
 *
 * ## 对象成员：直接写入，被插入打断才退回 scratch（消除双写）
 * 对象成员不能无条件直接追加到 `members_`：解析 `{"a":{"x":1},"b":2}` 时，
 * 内层对象的成员 `"x"` 会插在外层成员 `"a"` 与 `"b"` 之间，外层成员就不再连续。
 *
 * 但「插入」只发生在**子容器闭合**那一刻——在那之前，本容器写过的直接成员
 * 一定紧贴 `members_` 池尾、彼此连续。因此：
 *
 * 1. 容器打开时记下池尾 `member_begin`，成员用 `PushMember` 直接追加（**只写一次**）；
 * 2. 每次写新成员前比较池尾是否仍等于 `member_begin + direct_count`；
 * 3. 一旦不等（子容器已把成员提交到池尾），把已写入的连续成员复制进
 *    `member_scratch_` 并转为 scratch 模式，闭合时整段提交；
 * 4. 从未被打断的容器（绝大多数字段扁平对象、以及子容器正好是最后一个成员的
 *    对象）闭合时**零拷贝**，`Span` 直接指向已经写好的成员池区段。
 *
 * 代价：被打断的容器在池里留下一段 `direct_count` 个成员的旧槽位（不回收、
 * 不计入 `MemberCount()`，`m_member_used` 继续前进），换取「无插入容器写一次」。
 *
 * 数组元素仍走「先收集、闭合时提交」的 scratch 路径（`element_scratch_`）：
 * 元素只有 4 B，且数组元素容器的插入时机与对象不同，收益不足以抵消复杂度。
 *
 * ## 生命周期与所有权
 * - `Document` 拥有全部节点数据，**不可拷贝、不可移动**（`Value` 句柄里存了
 *   指向它的指针，移动会让所有句柄悬垂）；
 * - 无转义字符串借用输入缓冲：`input` 必须比 `Document` 活得久；
 * - 转义字符串由 `strings_` 池持有；这个池在解析期间**不扩容**（首次写入前
 *   就按剩余输入长度预留），因此字符串视图在文档存活期内始终有效；
 * - `Document` 析构 = 归还一块 arena（或什么都不做），没有递归、没有每节点
 *   析构。10 万层嵌套的解析与析构都不会爆栈。
 *
 * ## 复用
 * `Parse` 入口会先 `Clear()`（池保留容量），因此同一个 `Document` 可以反复
 * 承载多次解析结果；**每次解析后旧 `Value` 句柄都会失效**。
 */

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/core/text.hpp"
#include "go2cpp/json/error.hpp"
#include "go2cpp/json/value.hpp"

namespace go2cpp::json {

template <typename Policy>
class BasicParser;

namespace detail {

class Scanner;

/// @brief 值种类标签（节点池里每值 1 B 标签）。
enum class NodeKind : std::uint8_t {
  kNull = 0,   ///< `null`
  kBool = 1,   ///< `true` / `false`
  kInt = 2,    ///< 落在 int64 范围的整数
  kDouble = 3, ///< 带小数/指数的浮点
  kDecimal = 4,///< 超出 int64/double 的数值（池化）
  kString = 5, ///< 字符串
  kArray = 6,  ///< 数组（span 指向 `elements_`）
  kObject = 7, ///< 对象（span 指向 `members_`）
};

/**
 * @brief 一个值节点：1 B 标签 + 24 B payload（最大分支是 `core::Text`）。
 *
 * @note 所有分支都是 trivially copyable 的平凡类型，节点池的 `push_back` 就是
 *       定长块复制，没有 variant 的类型切换、没有每节点析构。
 */
struct Node {
  /// @brief 容器子表区间：数组指向 `elements_`，对象指向 `members_`。
  struct Span {
    std::uint32_t begin = 0;  ///< 子表起始下标。
    std::uint32_t count = 0;  ///< 子表元素个数。
  };

  /// @brief 各分支的载荷；具体生效字段由 `kind` 决定。
  union Payload {
    bool boolean;                 ///< `kBool`
    std::int64_t integer;         ///< `kInt`
    double real;                  ///< `kDouble`
    std::uint32_t decimal_index;  ///< `kDecimal`：`decimals_` 下标
    Span span;                    ///< `kArray` / `kObject`
    core::Text text;              ///< `kString`：借用输入或指向字符串池

    /// @brief 默认构造**不写任何字节**：`kind` 决定哪个分支有效，活跃分支的
    ///        payload 由写者（解析器 / `Node(NodeKind)` 的调用方）随后整体写入。
    ///        解析器对每个值都会写满自己的分支（`Text` 24 B / `real` 8 B /
    ///        `Span` 8 B），因此这里零初始化只是每值 3~8 条白写的指令
    ///        （深嵌套 100 层实测：去掉后 retire 指令 -3.4%）。
    Payload() noexcept {}
  };

  Node() noexcept = default;

  /// @brief 构造指定种类的节点；payload 不初始化（见 `Payload` 的说明）。
  explicit Node(NodeKind value_kind) noexcept : kind(value_kind) {}

  /// @brief 「只写 kind、不清 payload」的构造标签（热路径专用）。
  struct BareKindTag {};

  /**
   * @brief 构造一个只初始化 `kind` 的节点槽；payload 保持池内旧字节。
   *
   * @param tag [in] 固定标签（区分整节点构造，避免与显式构造重载冲突）。
   * @param value_kind [in] 生效分支。
   *
   * @details 解析器对每个值都会**完整填写**它所在分支的 payload（字符串写
   *          `Text`、数字写 `integer`/`real`、容器在闭合时写 `Span`），因此
   *          24 B 的零初始化是纯粹的浪费。深嵌套/数字密集输入里这是每值
   *          6~8 条指令。调用方**必须**保证随后写入本分支 payload，否则读到
   *          的是池内残留字节。
   */
  Node(BareKindTag /*tag*/, NodeKind value_kind) noexcept : kind(value_kind) {}

  NodeKind kind = NodeKind::kNull;  ///< 生效分支。
  Payload payload;                  ///< 分支载荷。
};

}  // namespace detail

/**
 * @brief JSON 对象成员：键视图 + 值节点下标。
 *
 * @details `key` 与 `value_index` 都落在 `Document::members_` 的连续区间里；
 *          对外通过 `Value::MemberName(i)` / `Value::At(i)` 访问，不需要直接
 *          使用本类型。
 */
struct Member {
  std::string_view key;              ///< 成员名（借用输入或指向字符串池）。
  std::uint32_t value_index = 0;     ///< 成员值在节点池中的下标。
};

/**
 * @brief 一次解析结果的全部存储（节点池 + 成员池 + 字符串池 + Decimal 池）。
 *
 * @details 线程安全：`Document` 本身不含同步；不同线程各自持有自己的文档即可。
 *          同一个 `Document` 上的读写必须由调用方串行化。
 */
class Document {
 public:
  /// @brief 构造空文档（`Root().IsNull() == true`）。
  Document() noexcept = default;
  /// @brief 析构：一次释放整块池，不递归遍历节点。
  ~Document() = default;

  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;
  Document(Document&&) = delete;
  Document& operator=(Document&&) = delete;

  /// @brief 根值句柄；空文档或解析失败时是 null 句柄（不绑定文档）。
  const Value& Root() const noexcept { return m_root; }

  /// @brief 节点池里的值节点数（含根与容器占位节点）。
  std::size_t ValueCount() const noexcept { return m_node_count; }
  /// @brief 已提交的对象成员数（所有对象累计）。
  std::size_t MemberCount() const noexcept { return m_member_count; }
  /// @brief 已提交的数组元素索引数（所有数组累计）。
  std::size_t ElementCount() const noexcept { return m_element_count; }
  /// @brief 转义字符串池的字节数。
  std::size_t StringBytes() const noexcept { return m_strings.size(); }
  /// @brief Decimal 池的元素数。
  std::size_t DecimalCount() const noexcept { return m_decimals.size(); }

  /**
   * @brief 本次解析中「被跳过的坏值」错误清单（按出现顺序）。
   *
   * @return 错误对象的只读视图；`kAbort`（默认）下**恒为空**，`kSkipValue`
   *         至多 1 条，`kCollect` 至多 `ParserConfig::max_errors` 条。
   *         调用方**不要**保存这个引用——下一次 `Parse` / `Clear` 会失效。
   *
   * @note 每条都是 `JsonError`，`Offset()/Line()/Column()` 指向坏值的出错字节；
   *       坏值在树里的位置已被填成 `null` 节点（成员/元素个数与顺序不变）。
   * @note 返回 true 的解析（`Root()` 可用）也可能带错误；这是「部分成功」，
   *       调用方应同时检查 `Root()` 与 `Errors()`。
   *
   * @note 默认 `kAbort` 路径从不分配错误表，返回的是一个进程级只读空表——
   *       这样 `Document` 的构造/析构在热路径上只多一个空指针检查。
   */
  const std::vector<ErrorPtr>& Errors() const noexcept {
    static const std::vector<ErrorPtr> kNoErrors;
    return m_errors ? *m_errors : kNoErrors;
  }

  /// @brief 已记录的坏值错误数；等价于 `Errors().size()`。
  std::size_t ErrorCount() const noexcept { return m_errors ? m_errors->size() : 0; }

  /// @brief 成员 scratch 的当前深度（解析器内部用；只有退回 scratch 收集的容器占用）。
  std::size_t MemberScratchSize() const noexcept { return m_member_scratch_count; }
  /// @brief 当前未提交的元素 scratch 深度（解析器内部用）。
  std::size_t ElementScratchSize() const noexcept { return m_element_scratch_count; }

  /// @brief 是否为「无结果」状态（根为 null）。
  bool Empty() const noexcept { return m_root.IsNull(); }

  /**
   * @brief 清空所有池并令根为 null；**保留容量**，便于同一文档反复解析。
   *
   * @note 清空后此前从本文档取得的任何 `Value` 句柄都失效。
   */
  void Clear() noexcept {
    m_node_count = 0;
    m_member_count = 0;
    m_member_used = 0;
    m_element_count = 0;
    m_member_scratch_count = 0;
    m_element_scratch_count = 0;
    m_strings.clear();
    m_decimals.clear();
    if (m_errors) {
      m_errors->clear();
    }
    m_root = Value();
  }

 private:
  friend class Value;
  template <typename Policy>
  friend class BasicParser;
  friend class detail::Scanner;

  // ---- 只读池访问（Value 用）------------------------------------------------

  /// @brief 取节点；调用方保证 `index < ValueCount()`。
  const detail::Node& NodeAt(std::uint32_t index) const noexcept { return m_nodes[index]; }

  /// @brief 取成员；调用方保证 `index < MemberCount()`。
  const Member& MemberAt(std::uint32_t index) const noexcept { return m_members[index]; }

  /// @brief 取数组元素头节点下标；调用方保证下标有效。
  std::uint32_t ElementAt(std::uint32_t index) const noexcept { return m_elements[index]; }

  /// @brief 取 Decimal；调用方保证下标有效。
  const core::Decimal& DecimalAt(std::uint32_t index) const noexcept {
    return m_decimals[index];
  }

  /// @brief 由池下标构造句柄。
  Value Handle(std::uint32_t index) const noexcept { return Value(this, index); }

  // ---- 写入池（解析器用）----------------------------------------------------

  /// @brief 追加一个节点并返回其池下标。
  std::uint32_t PushNode(detail::Node node) {
    if (m_node_count == m_node_capacity) {
      GrowPools();
    }
    ::new (static_cast<void*>(m_nodes + m_node_count)) detail::Node(node);
    return static_cast<std::uint32_t>(m_node_count++);
  }

  /// @brief 下一个节点将占用的池下标（即 `ValueCount()` 的 32 位形式）。
  std::uint32_t NodeBegin() const noexcept { return static_cast<std::uint32_t>(m_node_count); }

  /**
   * @brief 追加一个指定种类的节点槽并返回其引用，供调用方就地填写 payload。
   *
   * @param kind [in] 节点种类；调用方可随后改写（数字/字面量解析器会自行写 kind）。
   * @return 指向池内新节点的引用——**在下一次会触发 `GrowPools` 的写入前有效**
   *         （追加成员/元素/节点都可能搬迁 arena）。
   *
   * @details 与 `PushNode(Node)` 的区别只在于写次数：这里节点直接在池内构造，
   *          省掉「调用方栈上临时对象 → 按值传参 → placement new」的两次 32 B 拷贝。
   *          解析失败需要撤销时调用 `PopNode()`，让 `ValueCount()` 与旧行为一致。
   */
  detail::Node& PushNodeSlot(detail::NodeKind kind) {
    if (m_node_count == m_node_capacity) {
      GrowPools();
    }
    detail::Node* const slot = m_nodes + m_node_count;
    ++m_node_count;
    return *::new (static_cast<void*>(slot)) detail::Node(kind);
  }

  /// @brief 撤销最近一次 `PushNodeSlot`（失败路径专用；不析构已写内容）。
  /**
   * @brief 追加一个「只写 kind」的节点槽（热路径：调用方随后整体写 payload）。
   * @param kind [in] 节点种类。
   * @return 指向池内新节点的引用；有效性与 `PushNodeSlot` 相同（下一次可能
   *         触发 `GrowPools` 的写入前有效）。
   *
   * @details 与 `PushNodeSlot` 的区别：**不清 24 B payload**。解析器的三类
   *          写者都会覆盖 payload（字符串 `Text` 24 B、数字 8 B、容器 `Span`
   *          8 B），因此对最终结果没有影响；深嵌套每值省 3~8 条清零指令。
   */
  detail::Node& PushNodeSlotBare(detail::NodeKind kind) {
    if (m_node_count == m_node_capacity) {
      GrowPools();
    }
    detail::Node* const slot = m_nodes + m_node_count;
    ++m_node_count;
    return *::new (static_cast<void*>(slot)) detail::Node(detail::Node::BareKindTag{}, kind);
  }

  void PopNode() noexcept {
    if (m_node_count != 0) {
      --m_node_count;
    }
  }

  /// @brief 向当前帧的临时区追加一个对象成员（尚未提交到成员池）。
  void PushMemberScratch(std::string_view key, std::uint32_t value_index) {
    if (m_member_scratch_count == m_member_scratch_capacity) {
      GrowPools();
    }
    ::new (static_cast<void*>(m_member_scratch + m_member_scratch_count))
        Member{key, value_index};
    ++m_member_scratch_count;
  }

  /// @brief 成员池下一个可写槽位的下标（池占用，不等于已提交成员数）。
  std::uint32_t MemberBegin() const noexcept {
    return static_cast<std::uint32_t>(m_member_used);
  }

  /// @brief 直接向成员池追加一个成员（单写：用于成员区段不会被后续插入挤开的容器）。
  void PushMember(std::string_view key, std::uint32_t value_index) {
    if (m_member_used == m_member_capacity) {
      GrowPools();
    }
    ::new (static_cast<void*>(m_members + m_member_used)) Member{key, value_index};
    ++m_member_used;
  }

  /// @brief 把已直接写入成员池的 `count` 个成员记为「已提交」（容器闭合时调用）。
  void AdvanceMemberCount(std::uint32_t count) noexcept {
    m_member_count += count;
  }

  /**
   * @brief 把成员池 `[begin, begin + count)` 的成员复制进当前帧的 scratch 并返回其起点。
   *
   * @param begin [in] 成员池起始下标（本容器第一个直接成员）。
   * @param count [in] 已直接写入的成员数；调用方必须保证这些槽位属于本容器且连续。
   * @return 复制后在 `MemberScratchSize()` 中的起点（= 本帧 scratch 基址）。
   *
   * @details 用于「直接写入模式」被后续容器插入打断时：这些成员在被打断前一直
   *          连续位于池尾，复制到 scratch 后容器改用 scratch 收集，闭合时一次提交，
   *          语义与旧的双写路径完全一致。池中留下的旧槽位不回退（`m_member_used`
   *          继续增长），但**不计入** `MemberCount()`。
   */
  std::uint32_t BeginScratchFromMembers(std::uint32_t begin, std::uint32_t count) {
    const std::uint32_t mark = static_cast<std::uint32_t>(m_member_scratch_count);
    for (std::uint32_t index = 0; index < count; ++index) {
      // 每次重新取 m_members：PushMemberScratch 可能触发 GrowPools 搬迁整块 arena。
      PushMemberScratch(m_members[begin + index].key, m_members[begin + index].value_index);
    }
    return mark;
  }

  /// @brief 向当前帧的临时区追加一个数组元素索引（尚未提交到索引池）。
  void PushElementScratch(std::uint32_t value_index) {
    if (m_element_scratch_count == m_element_scratch_capacity) {
      GrowPools();
    }
    m_element_scratch[m_element_scratch_count++] = value_index;
  }

  /**
   * @brief 标记一次转义字符串解码的起点，返回池内起始偏移。
   *
   * @details 与 `AppendStringData` / `AppendStringChar` / `CloseString` 配套：
   *          扫描器边解码边写池，避免中间 `std::string` 的构造与逐段拷贝。
   *          首次调用会按本次解析输入长度预留容量，之后**不再扩容**。
   */
  std::size_t MarkString() {
    if (m_strings.capacity() < m_string_budget) {
      m_strings.reserve(m_string_budget);
    }
    return m_strings.size();
  }

  /// @brief 向字符串池追加一段字节（调用方必须已 `MarkString`）。
  void AppendStringData(std::string_view bytes) {
    m_strings.insert(m_strings.end(), bytes.begin(), bytes.end());
  }

  /// @brief 向字符串池追加一个解码字节（调用方必须已 `MarkString`）。
  void AppendStringChar(char byte) { m_strings.push_back(byte); }

  /// @brief 结束一次转义字符串解码，返回指向池内的池化拥有态 `Text`。
  core::Text CloseString(std::size_t mark) noexcept {
    const std::size_t length = m_strings.size() - mark;
    return core::Text::Owned(std::string_view(m_strings.data() + mark, length),
                             static_cast<std::uint32_t>(mark), static_cast<std::uint32_t>(length));
  }

  /**
   * @brief 一次性把字节追加到字符串池，返回池化拥有态 `Text`（便捷版本）。
   * @param bytes [in] 解码结果；借用，函数返回前不保留引用。
   * @return 指向 `m_strings` 池内的稳定文本。
   */
  core::Text AppendString(std::string_view bytes) {
    if (bytes.empty()) {
      return core::Text::Borrow(std::string_view{});
    }
    const std::size_t mark = MarkString();
    AppendStringData(bytes);
    return CloseString(mark);
  }

  /**
   * @brief 提交当前帧收集的成员，返回其在成员池中的连续区间起点。
   * @param scratch_begin [in] 当前帧在成员 scratch 中的起点（打开容器时记录）。
   * @return 提交后的成员池起始下标；区间长度 = 提交前 scratch 大小 − 起点。
   * @note 提交后 scratch 回滚到 `scratch_begin`，父帧继续使用更低区段。
   */
  std::uint32_t CommitMembers(std::size_t scratch_begin) {
    const std::size_t count = m_member_scratch_count - scratch_begin;
    // spill 回退时 used 里含被插入后废弃的槽位，单次 2× 扩容不一定够：循环到够为止。
    while (m_member_used + count > m_member_capacity) {
      GrowPools();
    }
    const std::uint32_t begin = static_cast<std::uint32_t>(m_member_used);
    for (std::size_t index = 0; index < count; ++index) {
      ::new (static_cast<void*>(m_members + begin + index))
          Member(m_member_scratch[scratch_begin + index]);
    }
    m_member_used += count;
    m_member_count += count;
    m_member_scratch_count = scratch_begin;
    return begin;
  }

  /**
   * @brief 提交当前帧收集的数组元素，返回其在元素池中的连续区间起点。
   * @param scratch_begin [in] 当前帧在元素 scratch 中的起点。
   * @return 提交后的元素池起始下标；scratch 回滚到 `scratch_begin`。
   */
  std::uint32_t CommitElements(std::size_t scratch_begin) {
    const std::size_t count = m_element_scratch_count - scratch_begin;
    if (m_element_count + count > m_element_capacity) {
      GrowPools();
    }
    const std::uint32_t begin = static_cast<std::uint32_t>(m_element_count);
    for (std::size_t index = 0; index < count; ++index) {
      m_elements[m_element_count + index] = m_element_scratch[scratch_begin + index];
    }
    m_element_count += count;
    m_element_scratch_count = scratch_begin;
    return begin;
  }

  /// @brief 追加一个 Decimal 并返回其池下标。
  std::uint32_t PushDecimal(core::Decimal value) {
    m_decimals.push_back(std::move(value));
    return static_cast<std::uint32_t>(m_decimals.size() - 1);
  }

  /// @brief 回填容器节点的 `Span`。
  void PatchSpan(std::uint32_t node_index, std::uint32_t begin, std::uint32_t count) noexcept {
    detail::Node& node = m_nodes[node_index];
    node.payload.span.begin = begin;
    node.payload.span.count = count;
  }

  /**
   * @brief 追加一条「坏值」错误（`kSkipValue` / `kCollect` 恢复路径专用）。
   * @param error [in] 错误对象；**所有权移交给本函数**，为 nullptr 时忽略。
   * @note 上限由调用方（解析器）按 `ParserConfig::max_errors` 判断后决定是否调用；
   *       本函数不做截断，便于调用方把「记录」与「是否已满」写成一处判断。
   */
  void PushError(ErrorPtr error) {
    if (error) {
      if (!m_errors) {
        m_errors = std::make_unique<std::vector<ErrorPtr>>();
      }
      m_errors->push_back(std::move(error));
    }
  }

  /**
   * @brief 把两级 scratch 回滚到指定深度（错误恢复时丢弃半成品子容器）。
   * @param member_size [in] 目标成员 scratch 深度；大于当前深度时保持原值。
   * @param element_size [in] 目标元素 scratch 深度；大于当前深度时保持原值。
   *
   * @details 正常路径靠 `CommitMembers` / `CommitElements` 在容器闭合时回滚；
   *          恢复路径**不会闭合**坏值容器帧，必须显式回滚，否则父容器闭合时
   *          会把子容器的临时成员误当成自己的直接成员提交（成员数翻倍、顺序错乱）。
   */
  void RollbackScratch(std::size_t member_size, std::size_t element_size) noexcept {
    m_member_scratch_count = (std::min)(member_size, m_member_scratch_count);
    m_element_scratch_count = (std::min)(element_size, m_element_scratch_count);
  }

  /// @brief 设置根节点下标。
  void SetRoot(std::uint32_t index) noexcept { m_root = Value(this, index); }

  /**
   * @brief 按输入长度预分配池容量（解析入口调用一次）。
   *
   * @details 节点 / 成员 / 两级 scratch / 数组索引**共用同一块 arena**，
   *          一次 `new[]` 完成，避免 5 次 malloc/free 往返。容量不足时
   *          `GrowPools` 按 2× 扩容并搬迁已用数据（所有句柄都是下标，安全）。
   */
  void ReserveFor(std::size_t input_size) {
    const std::size_t node_capacity = input_size / kBytesPerNode + kSlack;
    const std::size_t member_capacity = input_size / kBytesPerMember + kSlack;
    const std::size_t element_capacity = input_size / kBytesPerElement + kSlack;
    if (node_capacity > m_node_capacity || member_capacity > m_member_capacity ||
        member_capacity > m_member_scratch_capacity || element_capacity > m_element_capacity ||
        element_capacity > m_element_scratch_capacity) {
      AllocatePools((std::max)(node_capacity, m_node_capacity),
                    (std::max)(member_capacity, m_member_capacity),
                    (std::max)(member_capacity, m_member_scratch_capacity),
                    (std::max)(element_capacity, m_element_capacity),
                    (std::max)(element_capacity, m_element_scratch_capacity), true);
    }
    m_string_budget = input_size;
  }

  /// @brief 一次分配所有池的 arena 并切分指针；**不改变已用计数**。
  /// @param allow_inline [in] 允许使用对象内联 arena（GrowPools 迁移时必须为
  ///        false：新旧数据可能落在同一块内联区，会自覆盖）。
  void AllocatePools(std::size_t node_capacity, std::size_t member_capacity,
                     std::size_t scratch_capacity, std::size_t element_capacity,
                     std::size_t element_scratch_capacity, bool allow_inline) {
    const std::size_t node_bytes = node_capacity * sizeof(detail::Node);
    const std::size_t member_bytes = member_capacity * sizeof(Member);
    const std::size_t scratch_bytes = scratch_capacity * sizeof(Member);
    const std::size_t element_bytes = element_capacity * sizeof(std::uint32_t);
    const std::size_t element_scratch_bytes = element_scratch_capacity * sizeof(std::uint32_t);
    const std::size_t total = node_bytes + member_bytes + scratch_bytes + element_bytes +
                              element_scratch_bytes;
    char* base = nullptr;
    if (allow_inline && total <= kInlineArenaBytes) {
      // 小输入零堆分配：直接用对象内的对齐缓冲。
      m_arena.reset();
      base = reinterpret_cast<char*>(m_inline_arena);
    } else {
      m_arena.reset(new char[total]);
      base = m_arena.get();
    }
    m_nodes = reinterpret_cast<detail::Node*>(base);
    m_members = reinterpret_cast<Member*>(base + node_bytes);
    m_member_scratch = reinterpret_cast<Member*>(base + node_bytes + member_bytes);
    m_elements = reinterpret_cast<std::uint32_t*>(base + node_bytes + member_bytes + scratch_bytes);
    m_element_scratch = reinterpret_cast<std::uint32_t*>(
        base + node_bytes + member_bytes + scratch_bytes + element_bytes);
    m_node_capacity = node_capacity;
    m_member_capacity = member_capacity;
    m_member_scratch_capacity = scratch_capacity;
    m_element_capacity = element_capacity;
    m_element_scratch_capacity = element_scratch_capacity;
  }

  /// @brief 容量不足时按 2× 扩容并搬迁已用数据（深嵌套输入会走到这里）。
  void GrowPools() {
    const std::size_t node_capacity = m_node_capacity != 0 ? m_node_capacity * 2 : kSlack;
    const std::size_t member_capacity = m_member_capacity != 0 ? m_member_capacity * 2 : kSlack;
    const std::size_t scratch_capacity =
        m_member_scratch_capacity != 0 ? m_member_scratch_capacity * 2 : kSlack;
    const std::size_t element_capacity = m_element_capacity != 0 ? m_element_capacity * 2 : kSlack;
    const std::size_t element_scratch_capacity =
        m_element_scratch_capacity != 0 ? m_element_scratch_capacity * 2 : kSlack;

    detail::Node* const old_nodes = m_nodes;
    Member* const old_members = m_members;
    Member* const old_scratch = m_member_scratch;
    std::uint32_t* const old_elements = m_elements;
    std::uint32_t* const old_element_scratch = m_element_scratch;
    const std::size_t old_node_count = m_node_count;
    const std::size_t old_member_count = m_member_used;
    const std::size_t old_scratch_count = m_member_scratch_count;
    const std::size_t old_element_count = m_element_count;
    const std::size_t old_element_scratch_count = m_element_scratch_count;

    std::unique_ptr<char[]> old_arena = std::move(m_arena);
    AllocatePools(node_capacity, member_capacity, scratch_capacity, element_capacity,
                  element_scratch_capacity, false);
    if (old_node_count != 0) {
      std::memcpy(m_nodes, old_nodes, old_node_count * sizeof(detail::Node));
    }
    if (old_member_count != 0) {
      std::memcpy(m_members, old_members, old_member_count * sizeof(Member));
    }
    if (old_scratch_count != 0) {
      std::memcpy(m_member_scratch, old_scratch, old_scratch_count * sizeof(Member));
    }
    if (old_element_count != 0) {
      std::memcpy(m_elements, old_elements, old_element_count * sizeof(std::uint32_t));
    }
    if (old_element_scratch_count != 0) {
      std::memcpy(m_element_scratch, old_element_scratch,
                  old_element_scratch_count * sizeof(std::uint32_t));
    }
    m_node_count = old_node_count;
    m_member_used = old_member_count;
    m_member_scratch_count = old_scratch_count;
    m_element_count = old_element_count;
    m_element_scratch_count = old_element_scratch_count;
    (void)old_arena;
  }

  /// @brief 每个节点槽的「输入字节预算」：节点池扩容系数。
  static constexpr std::size_t kBytesPerNode = 16;
  /// @brief 每个成员槽的「输入字节预算」：成员池扩容系数。
  static constexpr std::size_t kBytesPerMember = 24;
  /// @brief 每个数组元素槽的「输入字节预算」：元素索引池扩容系数。
  static constexpr std::size_t kBytesPerElement = 8;
  /// @brief 小输入的固定余量，避免零容量反复扩容。
  static constexpr std::size_t kSlack = 8;

  /// @brief 内联 arena 容量：典型配置/API 响应（≤4 KiB）完全零堆分配。
  static constexpr std::size_t kInlineArenaBytes = 4096;

  alignas(64) unsigned char m_inline_arena[kInlineArenaBytes];  ///< 内联池存储（不预初始化）。
  std::unique_ptr<char[]> m_arena;                 ///< 池存储（nullptr = 用内联区）。
  detail::Node* m_nodes = nullptr;                 ///< 节点池。
  std::size_t m_node_count = 0;                    ///< 已用节点数。
  std::size_t m_node_capacity = 0;                 ///< 节点容量。
  Member* m_members = nullptr;                     ///< 对象成员池（已提交）。
  std::size_t m_member_count = 0;                  ///< 已提交成员数。
  std::size_t m_member_used = 0;                   ///< 成员池已占用槽位数（含被插入后废弃的槽）。
  std::size_t m_member_capacity = 0;               ///< 成员容量。
  Member* m_member_scratch = nullptr;              ///< 按帧分段的成员临时区。
  std::size_t m_member_scratch_count = 0;          ///< 临时区已用数。
  std::size_t m_member_scratch_capacity = 0;       ///< 临时区容量。
  std::uint32_t* m_elements = nullptr;             ///< 数组元素索引池（已提交）。
  std::size_t m_element_count = 0;                 ///< 已提交元素数。
  std::size_t m_element_capacity = 0;              ///< 元素容量。
  std::uint32_t* m_element_scratch = nullptr;      ///< 按帧分段的元素临时区。
  std::size_t m_element_scratch_count = 0;         ///< 元素临时区已用数。
  std::size_t m_element_scratch_capacity = 0;      ///< 元素临时区容量。
  std::vector<char> m_strings;                     ///< 转义字符串池（按需）。
  std::vector<core::Decimal> m_decimals;           ///< Decimal 池（按需）。
  std::size_t m_string_budget = 0;                 ///< 字符串池本次解析的容量预算。
  /// @brief 坏值错误清单（kSkipValue/kCollect）；**惰性分配**，默认路径恒为 nullptr。
  std::unique_ptr<std::vector<ErrorPtr>> m_errors;
  Value m_root;                                    ///< 根句柄。
};

// ===========================================================================
// Value 实现（需要 Document 完整定义）
// ===========================================================================

inline const detail::Node* Value::NodePtr() const noexcept {
  if (m_doc == nullptr || m_index >= m_doc->ValueCount()) {
    return nullptr;
  }
  return &m_doc->NodeAt(m_index);
}

inline bool Value::IsNull() const noexcept {
  const detail::Node* node = NodePtr();
  return node == nullptr || node->kind == detail::NodeKind::kNull;
}

inline bool Value::IsBool() const noexcept {
  const detail::Node* node = NodePtr();
  return node != nullptr && node->kind == detail::NodeKind::kBool;
}

inline bool Value::IsInt() const noexcept {
  const detail::Node* node = NodePtr();
  return node != nullptr && node->kind == detail::NodeKind::kInt;
}

inline bool Value::IsDouble() const noexcept {
  const detail::Node* node = NodePtr();
  return node != nullptr && node->kind == detail::NodeKind::kDouble;
}

inline bool Value::IsDecimal() const noexcept {
  const detail::Node* node = NodePtr();
  return node != nullptr && node->kind == detail::NodeKind::kDecimal;
}

inline bool Value::IsNumber() const noexcept {
  const detail::Node* node = NodePtr();
  if (node == nullptr) {
    return false;
  }
  const detail::NodeKind kind = node->kind;
  return kind == detail::NodeKind::kInt || kind == detail::NodeKind::kDouble ||
         kind == detail::NodeKind::kDecimal;
}

inline bool Value::IsString() const noexcept {
  const detail::Node* node = NodePtr();
  return node != nullptr && node->kind == detail::NodeKind::kString;
}

inline bool Value::IsArray() const noexcept {
  const detail::Node* node = NodePtr();
  return node != nullptr && node->kind == detail::NodeKind::kArray;
}

inline bool Value::IsObject() const noexcept {
  const detail::Node* node = NodePtr();
  return node != nullptr && node->kind == detail::NodeKind::kObject;
}

inline std::size_t Value::Size() const noexcept {
  const detail::Node* node = NodePtr();
  if (node == nullptr) {
    return 0;
  }
  if (node->kind == detail::NodeKind::kArray || node->kind == detail::NodeKind::kObject) {
    return node->payload.span.count;
  }
  return 0;
}

inline Value Value::At(std::size_t index) const {
  const detail::Node* node = NodePtr();
  if (node == nullptr) {
    return Value();
  }
  if (index >= node->payload.span.count) {
    return Value();
  }
  const std::uint32_t offset = static_cast<std::uint32_t>(index);
  if (node->kind == detail::NodeKind::kArray) {
    return m_doc->Handle(m_doc->ElementAt(node->payload.span.begin + offset));
  }
  if (node->kind == detail::NodeKind::kObject) {
    return m_doc->Handle(m_doc->MemberAt(node->payload.span.begin + offset).value_index);
  }
  return Value();
}

inline Value Value::Find(std::string_view key) const noexcept {
  const detail::Node* node = NodePtr();
  if (node == nullptr || node->kind != detail::NodeKind::kObject) {
    return Value();
  }
  const std::uint32_t begin = node->payload.span.begin;
  const std::uint32_t count = node->payload.span.count;
  for (std::uint32_t offset = 0; offset < count; ++offset) {
    const Member& member = m_doc->MemberAt(begin + offset);
    if (member.key == key) {
      return m_doc->Handle(member.value_index);
    }
  }
  return Value();
}

inline Value Value::operator[](std::string_view key) const noexcept { return Find(key); }

inline std::string_view Value::MemberName(std::size_t index) const noexcept {
  const detail::Node* node = NodePtr();
  if (node == nullptr || node->kind != detail::NodeKind::kObject ||
      index >= node->payload.span.count) {
    return std::string_view{};
  }
  return m_doc->MemberAt(node->payload.span.begin + static_cast<std::uint32_t>(index)).key;
}

inline std::string_view Value::AsText() const noexcept {
  const detail::Node* node = NodePtr();
  if (node == nullptr || node->kind != detail::NodeKind::kString) {
    return std::string_view{};
  }
  return node->payload.text.view();
}

namespace detail {

/// @brief 从 `ItemValue` 取出自定义类型（`shared_ptr<const void>` 分支）。
template <typename T>
bool ExtractCustom(const core::ItemValue& item, T& out) {
  const auto* holder = std::get_if<std::shared_ptr<const void>>(&item);
  if (holder == nullptr || !*holder) {
    return false;
  }
  out = *static_cast<const T*>(holder->get());
  return true;
}

/// @brief 字符串 → 自定义类型：走 `core::Convert` 注册表。
template <typename T>
bool ConvertCustom(std::string_view text, T& out) {
  core::ItemValue item;
  if (!core::Convert(go2cpp::reflect::TypeIdOf<T>(), text, item)) {
    return false;
  }
  return ExtractCustom(item, out);
}

/// @brief 严格整数字符串解析：整串必须消耗完，范围检查由 `from_chars` 完成。
template <typename T>
bool ParseIntegerText(std::string_view text, T& out) {
  if (text.empty()) {
    return false;
  }
  T value{};
  const char* begin = text.data();
  const char* end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  out = value;
  return true;
}

/// @brief 把 double 精确往返地格式化成文本，供 Decimal 转换使用。
inline std::string DoubleToText(double value) {
  char buffer[32];
  const int length = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  if (length <= 0) {
    return std::string{};
  }
  return std::string(buffer, static_cast<std::size_t>(length));
}

}  // namespace detail

template <typename T>
bool Value::As(T& out) const {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  const detail::Node* node = NodePtr();
  const detail::NodeKind kind = node != nullptr ? node->kind : detail::NodeKind::kNull;

  if constexpr (std::is_same_v<U, std::nullptr_t>) {
    if (kind != detail::NodeKind::kNull) {
      return false;
    }
    out = nullptr;
    return true;
  } else if constexpr (std::is_same_v<U, bool>) {
    if (kind == detail::NodeKind::kBool) {
      out = node->payload.boolean;
      return true;
    }
    if (kind == detail::NodeKind::kInt) {
      if (node->payload.integer != 0 && node->payload.integer != 1) {
        return false;
      }
      out = node->payload.integer == 1;
      return true;
    }
    if (kind == detail::NodeKind::kString) {
      return core::StringToBool(AsText(), out);
    }
    return false;
  } else if constexpr (std::is_same_v<U, std::string>) {
    if (kind != detail::NodeKind::kString) {
      return false;
    }
    out.assign(AsText());
    return true;
  } else if constexpr (std::is_same_v<U, std::string_view>) {
    if (kind != detail::NodeKind::kString) {
      return false;
    }
    out = AsText();
    return true;
  } else if constexpr (std::is_same_v<U, core::Text>) {
    if (kind != detail::NodeKind::kString) {
      return false;
    }
    out = node->payload.text;
    return true;
  } else if constexpr (std::is_same_v<U, core::Decimal>) {
    if (kind == detail::NodeKind::kDecimal) {
      out = m_doc->DecimalAt(node->payload.decimal_index);
      return true;
    }
    if (kind == detail::NodeKind::kInt) {
      return core::StringToDecimal(std::to_string(node->payload.integer), out);
    }
    if (kind == detail::NodeKind::kDouble) {
      return core::StringToDecimal(detail::DoubleToText(node->payload.real), out);
    }
    if (kind == detail::NodeKind::kString) {
      return core::StringToDecimal(AsText(), out);
    }
    return false;
  } else if constexpr (std::is_integral_v<U>) {
    if (kind == detail::NodeKind::kInt) {
      const std::int64_t value = node->payload.integer;
      if constexpr (std::is_signed_v<U>) {
        if (value < static_cast<std::int64_t>(std::numeric_limits<U>::min()) ||
            value > static_cast<std::int64_t>(std::numeric_limits<U>::max())) {
          return false;
        }
      } else {
        if (value < 0 ||
            static_cast<std::uint64_t>(value) >
                static_cast<std::uint64_t>(std::numeric_limits<U>::max())) {
          return false;
        }
      }
      out = static_cast<U>(value);
      return true;
    }
    if (kind == detail::NodeKind::kDouble) {
      const double value = node->payload.real;
      if (!std::isfinite(value) || std::trunc(value) != value) {
        return false;
      }
      if (value < static_cast<double>(std::numeric_limits<U>::min()) ||
          value > static_cast<double>(std::numeric_limits<U>::max())) {
        return false;
      }
      out = static_cast<U>(value);
      return true;
    }
    if (kind == detail::NodeKind::kString) {
      return detail::ParseIntegerText(AsText(), out);
    }
    return false;
  } else if constexpr (std::is_floating_point_v<U>) {
    if (kind == detail::NodeKind::kDouble) {
      out = static_cast<U>(node->payload.real);
      return true;
    }
    if (kind == detail::NodeKind::kInt) {
      out = static_cast<U>(node->payload.integer);
      return true;
    }
    if (kind == detail::NodeKind::kString) {
      double parsed = 0.0;
      if (!core::StringToDouble(AsText(), parsed)) {
        return false;
      }
      out = static_cast<U>(parsed);
      return true;
    }
    return false;
  } else {
    if (kind != detail::NodeKind::kString) {
      return false;
    }
    return detail::ConvertCustom(AsText(), out);
  }
}

template <typename T>
T Value::As() const {
  T out{};
  (void)As(out);
  return out;
}

// ===========================================================================
// 尺寸契约
// ===========================================================================

static_assert(sizeof(Value) == 16, "Value 必须是 16 B 的文档句柄");
static_assert(sizeof(Member) == 24, "Member 必须是 24 B（key 16 + index 4 + padding 4）");

}  // namespace go2cpp::json
