#pragma once

/**
 * @file keystore.hpp
 * @brief 通用密钥管理：多来源加载、脱敏、常量时间比较与轮换过渡窗口。
 * @details 设计取舍与约束：
 *
 *          - **固定长度**：只接受 16B（AES-128 级）或 32B（PSK/AES-256 级）。
 *            其他长度在加载阶段拒绝，避免把口令、配置片段或任意材料误当
 *            密钥使用。长度校验先于任何密码学操作。
 *          - **无第三方依赖**：hex / base64 解码只用标准库实现，和 Bento
 *            其余模块「可单独拷走」的目标一致；不引入 OpenSSL。
 *          - **不泄漏明文**：所有错误文本与 `LogSink` 日志只包含来源、
 *            长度与 `Masked()` 掩码，绝不拼接密钥本身。加载失败的输入也
 *            不会出现在错误里（防止把「错误日志」变成密钥泄漏渠道）。
 *          - **轮换过渡**：`Rotate()` 保留上一代密钥一个可配置窗口
 *            （默认 5 分钟），窗口内 `Accepts()` 同时接受新旧密钥，便于
 *            对端逐步换 key；窗口外 `Previous()` 返回空 Key，旧密钥不再
 *            可用。窗口按 `std::chrono::steady_clock` 计算，不受系统时间
 *            调整影响。
 *          - **常量时间比较**：`ConstantTimeEquals()` 对内容不做短路，
 *            用于校验对端提交的密钥是否匹配，避免按字节比较的时序侧信道。
 *            长度不同的分支不可避免（长度不是秘密），但内容比较总是走完
 *            两个输入的最大长度。
 *
 *          线程模型：`KeyStore` 是配置期对象，内部不加锁。加载/轮换与读取
 *          必须由调用方串行化；加载成功后 `Current()` 保持有效，直到下一次
 *          成功加载或 `Rotate()`。加载失败不会破坏已加载的密钥。
 *
 *          集成方式：`KeyStore::Key` 提供 `data()/size()`，可直接传给
 *          `PeerHub::SetPreSharedKey` / `Link::SetPreSharedKey`。为避免
 *          `keystore` 反向依赖传输层，另提供头文件模板
 *          `go2cpp::bento::ApplyPreSharedKey(target, key)`，对任何具有
 *          `SetPreSharedKey(const std::uint8_t*, std::size_t)` 的对象都可用。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/bento/error/error.hpp"

namespace go2cpp::bento {

/// AES-128 级密钥字节数。
inline constexpr std::size_t kKeyBytes128 = 16;

/// PSK / AES-256 级密钥字节数。
inline constexpr std::size_t kKeyBytes256 = 32;

/// 本模块接受的最大密钥字节数。
inline constexpr std::size_t kKeyMaxBytes = kKeyBytes256;

/// @brief 密钥存储：编码加载、脱敏、比较与轮换。
/// @note 非线程安全；作为配置期对象使用。
class KeyStore final {
   public:
    /// @brief 单个密钥。默认构造为空 Key（`valid() == false`）。
    /// @details 内部按 32B 定长存储，拷贝是值语义。`data()`/`ToHex()` 会暴露
    ///          明文，仅用于传给密码学 API 或持久化；**禁止**写入日志。
    class Key final {
       public:
        Key() = default;

        /// @brief 从字节构造；长度不是 16/32 或 data 为空时返回空 Key。
        static Key FromBytes(const std::uint8_t* data, std::size_t bytes);

        /// @brief 是否持有合法长度的密钥。
        bool valid() const noexcept;

        /// @brief 密钥字节数；空 Key 返回 0。
        std::size_t size() const noexcept;

        /// @brief 明文指针；空 Key 返回内部零缓冲区的地址（不应读取）。
        const std::uint8_t* data() const noexcept;

        /// @brief 小写十六进制全文。**敏感**：只用于测试/持久化，禁止打日志。
        std::string ToHex() const;

        /// @brief 脱敏显示：保留十六进制前 4 后 4 个字符，其余替换为 `*`。
        /// @details 长度与 `ToHex()` 相同（每字节 2 个字符）；空 Key 返回
        ///          `"<empty>"`。该结果可以安全写入日志。
        std::string Masked() const;

       private:
        std::array<std::uint8_t, kKeyMaxBytes> m_bytes{};
        std::size_t m_bytes_used = 0;
    };

    /// @brief KeyStore 配置。
    struct Config {
        /// 轮换后旧密钥的保留窗口（毫秒）。0 表示旧密钥立即失效。
        std::uint64_t rotation_retention_ms = 5 * 60 * 1000;
        /// 加载文件时检查 group/other 权限；0644 等宽松权限只告警不拒绝。
        bool check_file_permissions = true;
        /// 单次加载允许的最大编码文本字节数（文件/环境变量/字符串共用）。
        std::size_t max_input_bytes = 4096;
    };

    /// @brief 日志回调。参数是已脱敏的一行文本，回调不得抛异常。
    using LogSink = std::function<void(std::string_view)>;

    KeyStore();
    explicit KeyStore(Config config);

    KeyStore(const KeyStore&) = delete;
    KeyStore& operator=(const KeyStore&) = delete;

    // ---- 加载 ----

    /// @brief 从字符串加载（hex 或 base64，可带 `hex:`/`base64:` 前缀）。
    /// @details 忽略 ASCII 空白；无前缀时，输入整体是偶数长度 hex 字符就按
    ///          hex，否则按 base64。全 hex 字符的 base64 串需加 `base64:`
    ///          前缀消除歧义。成功返回 nullptr 并更新 `Current()`/清空
    ///          `Previous()`；失败返回 `bento::error::ErrorPtr`，已加载的
    ///          密钥保持不变。
    error::ErrorPtr LoadFromString(std::string_view text);

    /// @brief 从文件加载编码文本。
    /// @details 文件权限（group/other 可读写）比 0600 宽松时追加一条 warning
    ///          并写日志，但**仍然加载**——拒绝启动会让运维更难恢复。
    /// @return 失败返回错误（文件不存在/不可读/非普通文件/过大/编码非法）。
    error::ErrorPtr LoadFromFile(const std::string& path);

    /// @brief 从环境变量加载编码文本（12-factor 部署常用）。
    /// @return 变量未设置/为空/编码非法时返回错误。
    error::ErrorPtr LoadFromEnv(const std::string& variable);

    // ---- 查询 ----

    /// @brief 是否已成功加载密钥。
    bool loaded() const noexcept;

    /// @brief 当前密钥（未加载时为空 Key）。
    const Key& Current() const noexcept;

    /// @brief 轮换窗口内的上一代密钥；窗口外或未轮换过返回空 Key。
    const Key& Previous() const noexcept;

    /// @brief 上一代密钥当前是否仍在保留窗口内。
    bool HasUsablePrevious() const noexcept;

    /// @brief 候选密钥是否匹配当前密钥或仍在窗口内的上一代密钥。
    /// @note 比较走 `ConstantTimeEquals`，不会按内容提前返回。
    bool Accepts(const Key& candidate) const noexcept;

    // ---- 轮换 ----

    /// @brief 设置新密钥，把旧密钥放入保留窗口。
    /// @details 新 Key 无效时返回 false 且不改变状态。若此前未加载，
    ///          直接成为 `Current()`（不产生 `Previous()`）。重复设置相同
    ///          密钥会刷新保留窗口，调用方应避免无意义的重复轮换。
    bool Rotate(const Key& new_key);

    /// @brief 修改旧密钥保留窗口（立即生效）。
    void SetRotationRetentionMs(std::uint64_t value) noexcept;

    /// @brief 当前保留窗口毫秒数。
    std::uint64_t rotation_retention_ms() const noexcept;

    // ---- 观测 ----

    /// @brief 注入日志回调（传空 `LogSink` 卸载）。
    void SetLogSink(LogSink sink);

    /// @brief 累计的非致命告警（目前是文件权限告警）。
    const std::vector<std::string>& warnings() const noexcept;

    /// @brief 清空告警，便于测试隔离。
    void ClearWarnings() noexcept;

    // ---- 常量时间比较 ----

    /// @brief 常量时间比较两个 Key；两者均无效视为相等。
    static bool ConstantTimeEquals(const Key& a, const Key& b) noexcept;

    /// @brief 常量时间比较任意字节串。
    /// @details 内容比较不短路，总是执行 `max(a_bytes, b_bytes)` 轮；
    ///          长度不等时返回 false。任一指针为空且对应长度非 0 返回 false。
    static bool ConstantTimeEquals(const std::uint8_t* a, std::size_t a_bytes,
                                   const std::uint8_t* b, std::size_t b_bytes) noexcept;

   private:
    /// 解析并原子替换当前密钥；失败不修改状态。
    error::ErrorPtr LoadDecoded(std::string_view text, const char* source);
    void AddWarning(std::string message);
    void Emit(std::string_view message) const;

    Config m_config;
    Key m_current;
    Key m_previous;
    std::uint64_t m_rotated_at_ms = 0;
    std::vector<std::string> m_warnings;
    LogSink m_log_sink;
    mutable std::uint64_t m_sink_failures = 0;
};

/// @brief 把 KeyStore::Key 应用到任何提供 `SetPreSharedKey(data, size)` 的对象。
/// @details 用于 `PeerHub` / `Link`，无需让 keystore 依赖传输层头文件。
///          16B Key 对这两个传输对象会返回 false（它们要求 32B PSK）。
template <typename Target>
bool ApplyPreSharedKey(Target& target, const KeyStore::Key& key)
{
    if (!key.valid()) {
        return false;
    }
    return target.SetPreSharedKey(key.data(), key.size());
}

}  // namespace go2cpp::bento
