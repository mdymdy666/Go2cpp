/**
 * @file json.hpp
 * @brief JSON 的 Go2Cpp 边界层：深度栅栏 + 自研 JSON 引擎之上的有序值类型。
 *
 * @details 历史上本文件是第三方 JSON 单头的薄封装（`using Json = 上游类型`）。
 *          现已迁移到项目自研引擎（`include/go2cpp/json/`）：
 *
 *          - **解析**：`go2cpp::json::Parser`（扁平池 + 显式栈，深文档不爆栈）；
 *          - **序列化**：`go2cpp::json::JsonBuilder`（转义 / 数字格式的唯一实现）；
 *          - **值容器**：本文件的 `Json`——对象用 `std::map`（**键按字典序**，
 *            与迁移前的上游默认行为一致，保证输出可逐字节对比），
 *            数组用 `std::vector`，标量用 `std::variant`。
 *
 *          `Json` 只保留 debug / CLI / bench 实际用到的 DOM 子集（构造、下标、
 *          `value` / `get` / `contains` / 数组遍历 / `dump`），不追求上游完整性。
 *
 * ## 为什么仍需要入口栅栏
 *
 * 1. **解析之后的递归操作才是风险点。** 自研解析器本身用显式栈，但
 *    `Json` 的 `dump()`（本文件）与 `Json` 的拷贝是递归的；Fiber 栈固定
 *    128 KiB、栈底只有一页 guard page，越界即进程被杀（退出码 139）。
 *    因此深度上限必须挡在「外部不可信文本」进入程序之前。
 * 2. **深嵌套是资源放大器。** 解析所需堆内存与时间 O(嵌套深度)，不设上限
 *    等于把成本交给输入方（DoS 面）。
 *
 * `ParseJson` 正是针对这两点：先用**迭代**扫描器（不递归、不分配）量出
 * 嵌套深度，超限立即返回可处理的 `ErrorPtr`，未超限才进入自研解析器。
 *
 * @note 本头文件**不进入** `go2cpp/runtime.hpp` 聚合头：JSON 并非运行时语义
 *       的一部分，需要 JSON 的翻译单元显式包含 `go2cpp/json.hpp` 即可。
 * @note 本文件是 header-only：没有对应 .cpp，也不需要新增 CMake target
 *       （它依赖的 `go2cpp_json` 由使用方链接）。
 */

#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "go2cpp/error.hpp"
#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/document.hpp"
#include "go2cpp/json/json_build.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/value.hpp"

namespace go2cpp {

/**
 * @brief `Json::parse` 失败时抛出的异常。
 *
 * @details 与旧实现（上游的 `parse_error`）一样派生自 `std::exception`，
 *          调用方用 `catch (const std::exception&)` 即可；`what()` 带上
 *          「第几个字节」等定位信息，便于 CLI 报错。
 */
class JsonParseError : public std::runtime_error {
public:
    explicit JsonParseError(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * @brief 有序 JSON 值（对象键按字典序，与迁移前的输出字节一致）。
 *
 * ## 定位
 * debug 视图 / DebugServer 响应 / CLI 输出的**唯一**值容器；解析入口是
 * `ParseJson`（自研解析器），序列化出口是 `Json::dump`（内部用
 * `go2cpp::json::JsonBuilder` 完成转义与数字格式化）。
 *
 * ## 并发/失败语义
 * - 值语义：可拷贝、可移动；拷贝是深拷贝（递归，深度受入口栅栏约束）；
 * - 非 const `operator[]` 在 key 不存在时**插入 null**（与上游一致），
 *   因此不要拿它做只读查询——用 `find` / `contains` / `value`；
 * - const `operator[]` 不插入，缺失返回全局 null 引用；
 * - `parse` 失败抛 `JsonParseError`；`dump` 不做错误处理（见 JsonBuilder 的
 *   数值契约：只可能因 NaN/Inf 失败，而这类值无法从解析产生）。
 */
class Json {
public:
    using array_t = std::vector<Json>;
    using object_t = std::map<std::string, Json, std::less<>>;
    using initializer_list_t = std::initializer_list<std::pair<const std::string, Json>>;

    Json() noexcept : m_value(nullptr) {}
    Json(std::nullptr_t) noexcept : m_value(nullptr) {}
    Json(bool value) : m_value(value) {}
    Json(double value) : m_value(value) {}
    Json(const char* value) : m_value(std::string(value == nullptr ? "" : value)) {}
    Json(std::string value) : m_value(std::move(value)) {}
    Json(std::string_view value) : m_value(std::string(value)) {}
    Json(const array_t& value) : m_value(value) {}
    Json(array_t&& value) : m_value(std::move(value)) {}
    Json(const object_t& value) : m_value(value) {}
    Json(object_t&& value) : m_value(std::move(value)) {}
    /// @brief 对象字面量构造：`Json m = {{"k", v}, ...}`（与旧实现一致）。
    Json(initializer_list_t init) : m_value(object_t{}) {
        auto& object = std::get<object_t>(m_value);
        for (const auto& entry : init) {
            object.emplace(entry.first, entry.second);
        }
    }
    /// @brief 任意可转 JSON 的数组 → 数组（`std::vector<std::string>` 等）。
    template <typename T, typename = std::enable_if_t<!std::is_same_v<T, Json>>>
    Json(const std::vector<T>& value) : m_value(array_t{}) {
        auto& array = std::get<array_t>(m_value);
        array.reserve(value.size());
        for (const auto& item : value) {
            array.push_back(Json(item));
        }
    }
    /// @brief 简单字符串表 → 对象（CLI 的 `status` 键值对常用）。
    Json(const std::map<std::string, std::string>& value) : m_value(object_t{}) {
        auto& object = std::get<object_t>(m_value);
        for (const auto& entry : value) {
            object.emplace(entry.first, Json(entry.second));
        }
    }

    /// @brief 整型构造：有符号进 int64 节点，无符号进 uint64 节点（与旧实现一致）。
    template <typename T,
              typename = std::enable_if_t<std::is_integral_v<T> &&
                                          !std::is_same_v<T, bool> &&
                                          !std::is_same_v<T, char>>,
              typename = void>
    Json(T value)  // NOLINT(google-explicit-constructor)：DOM 写法需要隐式转换
        : m_value(std::is_signed_v<T>
                      ? Storage{static_cast<std::int64_t>(value)}
                      : Storage{static_cast<std::uint64_t>(value)}) {}

    // -----------------------------------------------------------------
    // 工厂与解析
    // -----------------------------------------------------------------

    /// @brief 空对象 `{}`。
    static Json object(initializer_list_t init = {}) {
        Json out(object_t{});
        for (const auto& entry : init) {
            std::get<object_t>(out.m_value).emplace(entry.first, entry.second);
        }
        return out;
    }

    /// @brief 空数组 `[]`。
    static Json array(std::initializer_list<Json> init = {}) {
        Json out(array_t{});
        for (const auto& item : init) {
            std::get<array_t>(out.m_value).push_back(item);
        }
        return out;
    }

    /// @brief 解析 JSON 文本；失败抛 `JsonParseError`（不带深度栅栏，见 `ParseJson`）。
    static Json parse(std::string_view text) {
        go2cpp::json::Document document;
        go2cpp::json::Parser parser;
        go2cpp::json::ParserConfig config;
        go2cpp::ErrorPtr error;
        if (!parser.Parse(text, document, error, config)) {
            throw JsonParseError("JSON 解析失败: " + go2cpp::ErrorMessage(error));
        }
        return detail_from_value(document.Root());
    }

    /// @brief 不抛异常的解析重载（兼容旧的 `parse(text, nullptr, false)` 写法）。
    /// @note `allow_exceptions == false` 时失败返回 null；调用方用 `is_object()` 等判断。
    static Json parse(std::string_view text, std::nullptr_t /*unused*/, bool allow_exceptions) {
        go2cpp::json::Document document;
        go2cpp::json::Parser parser;
        go2cpp::json::ParserConfig config;
        go2cpp::ErrorPtr error;
        if (!parser.Parse(text, document, error, config)) {
            if (allow_exceptions) {
                throw JsonParseError("JSON 解析失败: " + go2cpp::ErrorMessage(error));
            }
            return Json();
        }
        return detail_from_value(document.Root());
    }

    /// @brief 解析失败在旧接口里用 discard 值表示；本实现用 null 表示，恒为 false。
    bool is_discarded() const noexcept { return false; }

    // -----------------------------------------------------------------
    // 类型判定
    // -----------------------------------------------------------------

    bool is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(m_value); }
    bool is_boolean() const noexcept { return std::holds_alternative<bool>(m_value); }
    bool is_number_integer() const noexcept {
        return std::holds_alternative<std::int64_t>(m_value) ||
               std::holds_alternative<std::uint64_t>(m_value);
    }
    bool is_number_unsigned() const noexcept {
        return std::holds_alternative<std::uint64_t>(m_value);
    }
    bool is_number_float() const noexcept { return std::holds_alternative<double>(m_value); }
    bool is_number() const noexcept { return is_number_integer() || is_number_float(); }
    bool is_string() const noexcept { return std::holds_alternative<std::string>(m_value); }
    bool is_array() const noexcept { return std::holds_alternative<array_t>(m_value); }
    bool is_object() const noexcept { return std::holds_alternative<object_t>(m_value); }

    // -----------------------------------------------------------------
    // 容器 / 字符串尺寸
    // -----------------------------------------------------------------

    /// @brief 元素个数：数组/对象返回 size，字符串返回字节数，其余返回 0。
    std::size_t size() const noexcept {
        if (const auto* array = std::get_if<array_t>(&m_value)) {
            return array->size();
        }
        if (const auto* object = std::get_if<object_t>(&m_value)) {
            return object->size();
        }
        if (const auto* text = std::get_if<std::string>(&m_value)) {
            return text->size();
        }
        return 0;
    }

    /// @brief `size() == 0`（标量恒为 true）。
    bool empty() const noexcept { return size() == 0; }

    // -----------------------------------------------------------------
    // 访问
    // -----------------------------------------------------------------

    /// @brief 读写的对象下标：不存在则插入 null；非对象调用会把它变成对象。
    Json& operator[](std::string_view key) {
        if (!is_object()) {
            m_value = object_t{};
        }
        return std::get<object_t>(m_value)[std::string(key)];
    }

    /// @brief 只读下标：缺失或非对象返回全局 null 引用（不插入）。
    const Json& operator[](std::string_view key) const noexcept {
        if (const auto* object = std::get_if<object_t>(&m_value)) {
            const auto found = object->find(key);
            if (found != object->end()) {
                return found->second;
            }
        }
        return null_json();
    }

    /// @brief 数组下标（越界时读写语义不同：写会扩容，读返回 null）。
    Json& operator[](std::size_t index) {
        if (!is_array()) {
            m_value = array_t{};
        }
        auto& array = std::get<array_t>(m_value);
        if (index >= array.size()) {
            array.resize(index + 1);
        }
        return array[index];
    }

    /// @brief 只读数组下标；越界返回 null 引用。
    const Json& operator[](std::size_t index) const noexcept {
        if (const auto* array = std::get_if<array_t>(&m_value)) {
            if (index < array->size()) {
                return (*array)[index];
            }
        }
        return null_json();
    }

    /// @brief 查成员；不存在返回 nullptr（不插入）。
    const Json* find(std::string_view key) const noexcept {
        if (const auto* object = std::get_if<object_t>(&m_value)) {
            const auto found = object->find(key);
            if (found != object->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    /// @brief 对象是否含 key（非对象一律 false）。
    bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }

    /// @brief 取成员，缺失/类型不符时返回 `default_value`。
    /// @note `T = Json` 时直接返回值（`value("k", Json::object())` 这种用法）。
    template <typename T>
    T value(std::string_view key, T default_value) const {
        const Json* const item = find(key);
        if (item == nullptr) {
            return default_value;
        }
        if constexpr (std::is_same_v<T, Json>) {
            return *item;
        } else {
            T out{};
            if (!item->get(out)) {
                return default_value;
            }
            return out;
        }
    }

    /// @brief 追加元素（非数组调用会先变成数组）。
    void push_back(Json item) {
        if (!is_array()) {
            m_value = array_t{};
        }
        std::get<array_t>(m_value).push_back(std::move(item));
    }

    /// @brief 取字符串：成功返回 true 并写 `out`；失败保持 `out` 原值。
    bool get(std::string& out) const {
        if (const auto* text = std::get_if<std::string>(&m_value)) {
            out = *text;
            return true;
        }
        return false;
    }
    bool get(std::string_view& out) const {
        if (const auto* text = std::get_if<std::string>(&m_value)) {
            out = *text;
            return true;
        }
        return false;
    }
    bool get(bool& out) const {
        if (const auto* flag = std::get_if<bool>(&m_value)) {
            out = *flag;
            return true;
        }
        return false;
    }
    bool get(double& out) const {
        if (const auto* number = std::get_if<double>(&m_value)) {
            out = *number;
            return true;
        }
        if (const auto* number = std::get_if<std::int64_t>(&m_value)) {
            out = static_cast<double>(*number);
            return true;
        }
        if (const auto* number = std::get_if<std::uint64_t>(&m_value)) {
            out = static_cast<double>(*number);
            return true;
        }
        return false;
    }
    template <typename T,
              typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>,
              typename = void>
    bool get(T& out) const {
        if (const auto* number = std::get_if<std::int64_t>(&m_value)) {
            out = static_cast<T>(*number);
            return true;
        }
        if (const auto* number = std::get_if<std::uint64_t>(&m_value)) {
            out = static_cast<T>(*number);
            return true;
        }
        if (const auto* number = std::get_if<double>(&m_value)) {
            out = static_cast<T>(*number);
            return true;
        }
        return false;
    }

    /// @brief `get(T&)` 的取回版本；失败返回 `T{}`。
    template <typename T>
    T get() const {
        T out{};
        (void)get(out);
        return out;
    }

    /// @brief 取字符串内容；非字符串返回序列化文本（显式，避免与比较运算二义）。
    std::string as_string() const {
        if (const auto* text = std::get_if<std::string>(&m_value)) {
            return *text;
        }
        return dump();
    }

    /// @brief 值相等：数值按数值比较，其它按类型 + 内容比较。
    friend bool operator==(const Json& lhs, const Json& rhs) { return lhs.equals(rhs); }
    friend bool operator!=(const Json& lhs, const Json& rhs) { return !lhs.equals(rhs); }

    bool equals(const Json& other) const {
        if (is_number() && other.is_number()) {
            if (is_number_float() || other.is_number_float()) {
                return as_double() == other.as_double();
            }
            if (is_number_unsigned() || other.is_number_unsigned()) {
                // 一负一正必然不等；同为非负时按 uint64 比较。
                const bool lhs_negative = std::get_if<std::int64_t>(&m_value) != nullptr &&
                                          std::get<std::int64_t>(m_value) < 0;
                const bool rhs_negative = std::get_if<std::int64_t>(&other.m_value) != nullptr &&
                                          std::get<std::int64_t>(other.m_value) < 0;
                if (lhs_negative || rhs_negative) {
                    return lhs_negative && rhs_negative &&
                           std::get<std::int64_t>(m_value) ==
                               std::get<std::int64_t>(other.m_value);
                }
                return as_uint64() == other.as_uint64();
            }
            return std::get<std::int64_t>(m_value) == std::get<std::int64_t>(other.m_value);
        }
        return m_value == other.m_value;
    }

    // -----------------------------------------------------------------
    // 遍历：数组给元素，对象给成员（`it.key()` / `it.value()`）
    // -----------------------------------------------------------------

    /**
     * @brief 统一迭代器：数组迭代元素，对象迭代成员。
     *
     * @details 只为「遍历」这一用途设计：`*it` / `it.value()` 返回成员值，
     *          `it.key()` 返回对象键（数组返回空串）。写操作请走下标。
     */
    class iterator {
    public:
        using array_iterator = array_t::const_iterator;
        using object_iterator = object_t::const_iterator;

        iterator() noexcept = default;

        const Json& operator*() const noexcept {
            return m_kind == Kind::kArray ? *m_array : m_object->second;
        }
        const Json* operator->() const noexcept { return &operator*(); }

        iterator& operator++() noexcept {
            if (m_kind == Kind::kArray) {
                ++m_array;
            } else {
                ++m_object;
            }
            return *this;
        }

        bool operator==(const iterator& other) const noexcept {
            if (m_kind != other.m_kind) {
                return false;
            }
            return m_kind == Kind::kArray ? m_array == other.m_array
                                          : m_object == other.m_object;
        }
        bool operator!=(const iterator& other) const noexcept { return !(*this == other); }

        /// @brief 对象键；数组迭代返回空串。
        const std::string& key() const noexcept {
            return m_kind == Kind::kObject ? m_object->first : empty_key();
        }
        /// @brief 当前值（同 `operator*`）。
        const Json& value() const noexcept { return *m_array; }

    private:
        friend class Json;
        enum class Kind : unsigned char { kArray, kObject };

        iterator(array_iterator position) noexcept : m_kind(Kind::kArray) {
            m_array = position;
        }
        iterator(object_iterator position) noexcept : m_kind(Kind::kObject) {
            m_array = array_iterator{};
            m_object = position;
        }
        static const std::string& empty_key() {
            static const std::string kEmpty;
            return kEmpty;
        }

        Kind m_kind{Kind::kArray};
        array_iterator m_array{};
        object_iterator m_object{};
    };

    /// @brief 对象键值对遍历用的轻量代理（`for (auto& [k, v] : j.items())` 的替代）。
    class items_proxy {
    public:
        explicit items_proxy(const object_t* object) noexcept : m_object(object) {}

        /// @brief 一个成员：`key()` 取键、`value()` 取值。
        class item {
        public:
            explicit item(object_t::const_iterator position) noexcept : m_it(position) {}
            const std::string& key() const noexcept { return m_it->first; }
            const Json& value() const noexcept { return m_it->second; }

        private:
            object_t::const_iterator m_it;
        };

        class iterator {
        public:
            explicit iterator(object_t::const_iterator position) noexcept : m_it(position) {}
            item operator*() const noexcept { return item(m_it); }
            iterator& operator++() noexcept {
                ++m_it;
                return *this;
            }
            bool operator==(const iterator& other) const noexcept { return m_it == other.m_it; }
            bool operator!=(const iterator& other) const noexcept { return m_it != other.m_it; }

        private:
            object_t::const_iterator m_it;
        };

        iterator begin() const noexcept { return iterator(m_object->begin()); }
        iterator end() const noexcept { return iterator(m_object->end()); }

    private:
        const object_t* m_object;
    };

    /// @brief 遍历对象成员（非对象返回空范围）。
    items_proxy items() const noexcept {
        static const object_t kEmpty;
        const auto* object = std::get_if<object_t>(&m_value);
        return items_proxy(object == nullptr ? &kEmpty : object);
    }

    using const_iterator = iterator;

    const_iterator begin() const noexcept {
        if (const auto* array = std::get_if<array_t>(&m_value)) {
            return iterator(array->begin());
        }
        if (const auto* object = std::get_if<object_t>(&m_value)) {
            return iterator(object->begin());
        }
        return iterator(empty_array().begin());
    }
    const_iterator end() const noexcept {
        if (const auto* array = std::get_if<array_t>(&m_value)) {
            return iterator(array->end());
        }
        if (const auto* object = std::get_if<object_t>(&m_value)) {
            return iterator(object->end());
        }
        return iterator(empty_array().end());
    }

    // -----------------------------------------------------------------
    // 序列化
    // -----------------------------------------------------------------

    /**
     * @brief 序列化为 JSON 文本。
     * @param indent [in] `<0` 紧凑单行（默认）；`>0` 每层缩进字节数。
     * @return JSON 文本；对象键按字典序输出。
     * @note 字符串转义与数字格式全部交给 `go2cpp::json::JsonBuilder`，本类
     *       只负责拼结构和缩进 —— 保证与解析器共享同一份格式契约。
     */
    std::string dump(int indent = -1) const {
        std::string out;
        append_to(out, indent, 0);
        return out;
    }

    /// @brief 自研 `Document`/`Value` → `Json`（显式栈迭代，深文档不爆栈）。
    static Json detail_from_value(const go2cpp::json::Value& root);

private:
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t,
                                 double, std::string, array_t, object_t>;

    static const Json& null_json() {
        static const Json kNull;
        return kNull;
    }
    static const array_t& empty_array() {
        static const array_t kEmpty;
        return kEmpty;
    }

    void append_to(std::string& out, int indent, int depth) const;

    double as_double() const noexcept {
        if (const auto* number = std::get_if<double>(&m_value)) {
            return *number;
        }
        if (const auto* number = std::get_if<std::int64_t>(&m_value)) {
            return static_cast<double>(*number);
        }
        return static_cast<double>(std::get<std::uint64_t>(m_value));
    }
    std::uint64_t as_uint64() const noexcept {
        if (const auto* number = std::get_if<std::uint64_t>(&m_value)) {
            return *number;
        }
        if (const auto* number = std::get_if<std::int64_t>(&m_value)) {
            return static_cast<std::uint64_t>(*number);
        }
        return static_cast<std::uint64_t>(std::get<double>(m_value));
    }

    Storage m_value;
};

namespace detail {

/// @brief 写一个 JSON 标量的文本（转义/数字格式都走 JsonBuilder）。
inline std::string EncodeScalar(std::string_view text) {
    go2cpp::json::JsonBuilder builder;
    builder.String(text);
    return builder.str();
}
inline std::string EncodeScalar(bool value) {
    return value ? "true" : "false";
}
inline std::string EncodeScalar(std::int64_t value) {
    go2cpp::json::JsonBuilder builder;
    builder.Int(value);
    return builder.str();
}
inline std::string EncodeScalar(std::uint64_t value) {
    go2cpp::json::JsonBuilder builder;
    builder.Uint(value);
    return builder.str();
}
inline std::string EncodeScalar(double value) {
    go2cpp::json::JsonBuilder builder;
    builder.Double(value);
    return builder.str();
}

/// @brief 追加缩进（`indent` 空格 × `depth` 层）。
inline void AppendIndent(std::string& out, int indent, int depth) {
    if (indent <= 0) {
        return;
    }
    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
}

}  // namespace detail

inline void Json::append_to(std::string& out, int indent, int depth) const {
    // step1: 标量直接编码（null/bool/整型/浮点/字符串）。
    if (std::holds_alternative<std::nullptr_t>(m_value)) {
        out.append("null");
        return;
    }
    if (const auto* flag = std::get_if<bool>(&m_value)) {
        out.append(detail::EncodeScalar(*flag));
        return;
    }
    if (const auto* number = std::get_if<std::int64_t>(&m_value)) {
        out.append(detail::EncodeScalar(*number));
        return;
    }
    if (const auto* number = std::get_if<std::uint64_t>(&m_value)) {
        out.append(detail::EncodeScalar(*number));
        return;
    }
    if (const auto* number = std::get_if<double>(&m_value)) {
        out.append(detail::EncodeScalar(*number));
        return;
    }
    if (const auto* text = std::get_if<std::string>(&m_value)) {
        out.append(detail::EncodeScalar(std::string_view(*text)));
        return;
    }
    // step2: 空容器保持紧凑（与旧实现 dump 一致）。
    if (const auto* array = std::get_if<array_t>(&m_value)) {
        if (array->empty()) {
            out.append("[]");
            return;
        }
        out.push_back('[');
        for (std::size_t index = 0; index < array->size(); ++index) {
            if (index != 0) {
                out.push_back(',');
            }
            if (indent > 0) {
                out.push_back('\n');
                detail::AppendIndent(out, indent, depth + 1);
            }
            (*array)[index].append_to(out, indent, depth + 1);
        }
        if (indent > 0) {
            out.push_back('\n');
            detail::AppendIndent(out, indent, depth);
        }
        out.push_back(']');
        return;
    }
    // step3: 对象按键（std::map 已排序）逐个输出。
    const auto& object = std::get<object_t>(m_value);
    if (object.empty()) {
        out.append("{}");
        return;
    }
    out.push_back('{');
    bool first = true;
    for (const auto& entry : object) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        if (indent > 0) {
            out.push_back('\n');
            detail::AppendIndent(out, indent, depth + 1);
        }
        out.append(detail::EncodeScalar(std::string_view(entry.first)));
        out.push_back(':');
        if (indent > 0) {
            out.push_back(' ');
        }
        entry.second.append_to(out, indent, depth + 1);
    }
    if (indent > 0) {
        out.push_back('\n');
        detail::AppendIndent(out, indent, depth);
    }
    out.push_back('}');
}

inline Json Json::detail_from_value(const go2cpp::json::Value& root) {
    // 标量直接转换；容器用显式栈展开（与旧实现的 ToNlohmann 同一思路）。
    if (!root.IsArray() && !root.IsObject()) {
        if (root.IsNull()) {
            return Json(nullptr);
        }
        if (root.IsBool()) {
            bool parsed = false;
            root.As(parsed);
            return Json(parsed);
        }
        if (root.IsInt()) {
            std::int64_t parsed = 0;
            root.As(parsed);
            return Json(parsed);
        }
        if (root.IsDouble()) {
            double parsed = 0.0;
            root.As(parsed);
            return Json(parsed);
        }
        if (root.IsDecimal()) {
            go2cpp::json::core::Decimal decimal;
            if (!root.As(decimal)) {
                return Json(nullptr);
            }
            return Json(std::strtod(decimal.ToString().c_str(), nullptr));
        }
        if (root.IsString()) {
            return Json(std::string(root.AsText()));
        }
        return Json(nullptr);
    }

    struct Frame {
        go2cpp::json::Value source;
        Json local;
        std::size_t next{0};
        std::string parent_key;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{root, root.IsArray() ? Json::array() : Json::object(), 0, {}});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next < frame.source.Size()) {
            const std::size_t index = frame.next++;
            const go2cpp::json::Value child = frame.source.At(index);
            std::string key;
            if (frame.source.IsObject()) {
                key.assign(frame.source.MemberName(index));
            }
            if (child.IsArray() || child.IsObject()) {
                stack.push_back(Frame{child,
                                      child.IsArray() ? Json::array() : Json::object(),
                                      0,
                                      std::move(key)});
            } else if (frame.local.is_array()) {
                frame.local.push_back(Json::detail_from_value(child));
            } else {
                frame.local[key] = Json::detail_from_value(child);
            }
            continue;
        }
        Json finished = std::move(frame.local);
        std::string key = std::move(frame.parent_key);
        const bool is_root = stack.size() == 1;
        stack.pop_back();
        if (is_root) {
            return finished;
        }
        Frame& parent = stack.back();
        if (parent.local.is_array()) {
            parent.local.push_back(std::move(finished));
        } else {
            parent.local[key] = std::move(finished);
        }
    }
    return Json(nullptr);
}

/**
 * @brief `ParseJson` 默认接受的嵌套深度上限。
 * @details 自研解析器本身是迭代的；这里限制的是**解析之后**的递归操作
 *          （`Json` 的拷贝 / `dump`）与 O(深度) 的资源放大。取 64 对
 *          128 KiB Fiber 栈留出足够余量。
 */
inline constexpr std::size_t kDefaultJsonMaxDepth = 64U;

/**
 * @brief `JsonScanDepth` 的「不早退」哨兵：完整扫描整段文本。
 */
inline constexpr std::size_t kJsonDepthUnlimited = static_cast<std::size_t>(-1);

/**
 * @brief 返回「嵌套深度超限」错误的进程级身份锚点。
 *
 * @details `ParseJson` 在超限时返回的是
 *          `Wrap(JsonDepthExceededError(), 具体说明)`，因此判断错误种类必须用
 *          **身份比较**（`Is(error, JsonDepthExceededError())`），
 *          不能用文本比较 —— 文本里带着具体的上限与扫描层数，每次都不同。
 */
inline ErrorPtr JsonDepthExceededError() {
    static const ErrorPtr* const kError = new ErrorPtr(
        NewError("json: nested depth exceeds the configured limit"));
    return *kError;
}

/**
 * @brief 迭代扫描 JSON 文本，返回观测到的最大嵌套深度。
 *
 * @details 实现是**单层循环 + 显式计数**，不递归、不分配，因此扫描本身与
 *          输入深度无关地安全（100 万层嵌套也没有额外栈开销）。扫描器跳过
 *          字符串字面量内部（包括 `\"` 转义），所以 `"[[[["` 这样的字符串
 *          内容不会被计入深度。
 *
 *          扫描器**不做语法校验**：括号不匹配、非法 UTF-8 都不会在这里报错，
 *          合法性交给自研解析器判断 —— 这里只回答「嵌套有多深」。
 *
 * @param text 待扫描文本（可以是任意字节串，不要求是合法 JSON）。
 * @param stop_at 早退阈值：一旦观测深度**超过**该值就立即返回（返回值只是
 *        已观测到的层数，是真实深度的**下界**）。
 * @return 观测到的最大嵌套深度；空输入或纯标量返回 0。
 */
inline std::size_t JsonScanDepth(std::string_view text,
                                 std::size_t stop_at = kJsonDepthUnlimited) noexcept {
    // step1: 两个计数器 + 两个状态位，整个扫描只用一个循环。
    std::size_t depth = 0;    // 当前尚未闭合的容器层数
    std::size_t deepest = 0;  // 历史最大层数
    bool in_string = false;   // 状态位：是否位于字符串字面量内部
    bool escaped = false;     // 状态位：字符串内是否刚读到反斜杠
    for (const char ch : text) {
        // step2: 字符串内部只关心转义与结束引号，括号不计深度。
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        // step3: 左括号加深度；超过阈值立即早退（返回值只是下界）。
        if (ch == '{' || ch == '[') {
            ++depth;
            if (depth > deepest) {
                deepest = depth;
                if (deepest > stop_at) {
                    return deepest;  // 已足以判定超限，无需扫完
                }
            }
            continue;
        }
        // step4: 右括号减深度；深度已为 0 时忽略（不做语法校验）。
        if ((ch == '}' || ch == ']') && depth > 0) {
            --depth;
        }
    }
    // step5: 返回历史最大深度。
    return deepest;
}

/**
 * @brief `ParseJson` 的结果：值 + 可检查的错误。
 * @details 与 `ChannelRecvResult<T>`、`FutureResult<T>` 同一种「结构化结果」
 *          风格：不使用异常表达解析失败。
 * @note 判定成功请用 `ok()`，**不要**用 `value.is_null()` —— 合法输入 `"null"`
 *       解析成功后 `value` 同样是 null。
 */
struct JsonParseResult {
    /** @brief 成功时的解析结果；失败时为 null。 */
    Json value{};
    /** @brief 失败原因：深度超限为 `Wrap(JsonDepthExceededError(), ...)`，
     *         语法/编码错误为 `NewError("JSON 解析失败: ...")`；成功时为空。 */
    ErrorPtr error;

    /** @brief 返回本次解析是否成功。 */
    bool ok() const noexcept { return !error; }
};

/**
 * @brief 带深度栅栏的 JSON 解析入口。
 *
 * @details 执行两步，顺序不可交换：
 *          1. `JsonScanDepth(text, max_depth)` 迭代求深度，超限即早退；
 *          2. 仅当未超限时才调用自研解析器。
 *
 * @param text 待解析文本；允许为空（空输入会走解析器报错路径，不抛异常）。
 * @param max_depth 允许的最大嵌套深度，默认 `kDefaultJsonMaxDepth`（64）。
 * @return 结构化结果；失败信息在 `result.error`，不抛异常。
 */
inline JsonParseResult ParseJson(std::string_view text,
                                 std::size_t max_depth = kDefaultJsonMaxDepth) {
    JsonParseResult result;
    // step1: 先用迭代扫描器量深度；超限直接返回可处理的错误。
    const std::size_t observed = JsonScanDepth(text, max_depth);
    if (observed > max_depth) {
        result.error =
            Wrap(JsonDepthExceededError(),
                 "JSON 嵌套深度超过上限 " + std::to_string(max_depth) +
                     "（扫描到第 " + std::to_string(observed) +
                     " 层即早退，完整深度未统计）");
        return result;
    }
    // step2: 确认不超限后才交给自研解析器；失败转成 ErrorPtr。
    go2cpp::json::Document document;
    go2cpp::json::Parser parser;
    go2cpp::json::ParserConfig config;
    config.max_depth = max_depth == kJsonDepthUnlimited ? 0 : max_depth;
    ErrorPtr parse_error;
    if (!parser.Parse(text, document, parse_error, config)) {
        result.error =
            NewError(std::string("JSON 解析失败: ") + ErrorMessage(parse_error));
        return result;
    }
    result.value = Json::detail_from_value(document.Root());
    return result;
}

}  // namespace go2cpp
