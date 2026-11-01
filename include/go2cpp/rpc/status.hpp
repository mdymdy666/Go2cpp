#pragma once

/**
 * @file status.hpp
 * @brief RPC 状态码与错误值：对齐 gRPC 的 16 个语义码。
 *
 * @details
 * 本模块的**所有**失败路径都用 `Status` 返回值表示，不依赖异常：
 * 调用方可以逐次检查，也可以在 `Client::Call` 的便捷重载里选择抛
 * `RpcError`（异常只出现在错误路径，成功路径零开销）。
 *
 * 错误码取值与 gRPC 完全对齐（0..15），这样与公开 gRPC 基准对比时，
 * 「超时」「取消」「对端不可用」的语义可以直接对照，不需要翻译表。
 * 新增码只能追加在 15 之后，不能重排——错误码可能写进日志与监控。
 *
 * ## 与项目错误体系的关系
 *
 * 本模块刻意不复用 `go2cpp::error::ErrorCode`（那套是进程内类型链错误，
 * 16 个码的边界面向文件/网络/参数，与 RPC 的 wire 语义不同）。需要并网时：
 * `go2cpp::NewError(status.ToString())` 只保留文本，或按 `StatusCode` 映射。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <stdexcept>
#include <string>

namespace go2cpp::rpc {

/**
 * @brief RPC 状态码（与 gRPC 对齐）。
 */
enum class StatusCode : std::uint8_t {
    kOk = 0,                 ///< 成功。
    kCancelled = 1,          ///< 调用被取消（本地或对端）。
    kUnknown = 2,            ///< 未知错误（没有更具体的码可用时）。
    kInvalidArgument = 3,    ///< 参数非法（含反序列化失败、方法名非法）。
    kDeadlineExceeded = 4,   ///< 超过 deadline（客户端超时）。
    kNotFound = 5,           ///< 服务或方法未注册。
    kAlreadyExists = 6,      ///< 资源已存在（注册冲突等）。
    kPermissionDenied = 7,   ///< 无权限。
    kResourceExhausted = 8,  ///< 资源耗尽（帧/流缓冲超限）。
    kFailedPrecondition = 9, ///< 前置条件不满足（连接未建立等）。
    kAborted = 10,           ///< 并发冲突导致中止。
    kOutOfRange = 11,        ///< 取值越界。
    kUnimplemented = 12,     ///< 未实现（协议版本/帧类型不支持）。
    kInternal = 13,          ///< 服务端内部错误（含结果序列化失败）。
    kUnavailable = 14,       ///< 对端不可用（连接关闭/断开/写失败）。
    kDataLoss = 15,          ///< 数据损坏（帧或载荷不完整）。
};

/**
 * @brief 状态码短名（日志/测试失败信息用），永不返回 nullptr。
 * @param code 状态码。
 * @return 稳定短名（例如 `"DEADLINE_EXCEEDED"`）。
 */
const char* StatusCodeName(StatusCode code) noexcept;

/**
 * @brief RPC 状态值：码 + 面向人的文本。
 *
 * @details 纯值类型；成功时 `message` 为空（线上不传空字符串的文本，
 *          只占 1 字节长度前缀）。
 */
struct Status {
    /// 状态码；`kOk` 表示成功。
    StatusCode code = StatusCode::kOk;
    /// 错误文本（成功时为空）。
    std::string message;

    /// @brief 构造成功状态。
    Status() = default;

    /**
     * @brief 用状态码构造（文本为空）。
     * @param status_code 状态码。
     *
     * @note 非 explicit：`return StatusCode::kInvalidArgument;` 可直接作为
     *       `Status` 返回值，handler 写法更短。
     */
    Status(StatusCode status_code) : code(status_code) {}  // NOLINT(google-explicit-constructor)

    /**
     * @brief 用状态码 + 文本构造。
     * @param status_code 状态码。
     * @param text 错误文本（成功时建议留空）。
     */
    Status(StatusCode status_code, std::string text)
        : code(status_code), message(std::move(text)) {}

    /// @brief 是否成功。
    bool ok() const noexcept { return code == StatusCode::kOk; }

    /// @brief 是否成功（`if (status)` 写法）。
    explicit operator bool() const noexcept { return ok(); }

    /// @brief 复位为成功并清空文本。
    void Clear() noexcept {
        code = StatusCode::kOk;
        message.clear();
    }

    /// @brief `"CODE: message"` 形式（成功时返回 `"OK"`）。
    std::string ToString() const;

    /// @brief 成功状态。
    static Status Ok() noexcept { return Status(); }

    /// @brief 构造错误状态。
    static Status Error(StatusCode status_code, std::string text = {}) {
        return Status(status_code, std::move(text));
    }
};

/**
 * @brief `Status` 的异常包装，供 `Client::Call` 的便捷重载使用。
 *
 * @details 只在失败路径抛出；不参与正常调用的控制流。捕获后可用
 *          `error.status().code` 做程序化判断。
 */
class RpcError : public std::runtime_error {
   public:
    /// @brief 用状态构造；`what()` 返回 `status.ToString()` 的副本。
    explicit RpcError(Status status);

    /// @brief 失败状态（含码与文本）。
    const Status& status() const noexcept { return m_status; }

   private:
    Status m_status;  ///< 失败状态。
};

}  // namespace go2cpp::rpc
