#pragma once

/**
 * @file file_transfer.hpp
 * @brief MOD-28：通用文件传输（64KB 分块、SHA-256 校验、重试 3 次、
 *        断点续传、路径沙箱与大小上限）。
 * @details 传输模型是「发送端产帧、接收端消帧」，不绑定具体链路：帧可以
 *          走 Bento 可靠流，也可以走任何自带完整性的信道。两端各有独立
 *          的值对象与状态机：
 *
 *          ```
 *          发送端                          接收端
 *          FileSender::Open(path,...)  →   FileHeader（含整文件 SHA-256）
 *          BuildChunkFrame(i)          →   FileChunk（含块级 SHA-256）
 *          ...                             ApplyChunk() 逐块写 .part 文件
 *          （收到 kChecksumMismatch 重发）  校验失败不推进 offset，累计重试
 *          （收到 kRetryExhausted 放弃）
 *                                          Finish() 校验整文件 SHA-256 → rename
 *          ```
 *
 * ## 断点续传
 *
 * 接收端把数据写进 `<target>.part`；`Open` 时若 `.part` 已存在，按
 * **块边界**恢复（`resume_offset = floor(part_size / chunk_size) * chunk_size`），
 * 尾部不足一块的残片会截掉。发送端用 `Seek(resume_offset)` 从对应块继续，
 * 或直接 `Open` 后用 `next_index()`。续传内容是否正确由 `Finish` 的整文件
 * 摘要兜底：不一致时删除 `.part` 并返回 `kChecksumMismatch`，调用方从头重传。
 *
 * ## 路径沙箱（默认拒绝）
 *
 * `ResolveTransferPath(root, relative)` 拒绝：
 * - 绝对路径（以 `/` 开头）；
 * - 任何 `..` 段（规范化在解析前完成，`a/../b` 也一样拒绝）；
 * - 反斜杠与冒号（避免 Windows 盘符/ADS 语义在跨平台场景下变成逃逸）；
 * - 空路径、超过 `max_path_bytes`、控制字符/NUL；
 * - 解析后（含符号链接）不在 root 之下的路径。
 *
 * ## 上限与重试
 *
 * | 项 | 默认 | 硬上限 |
 * |---|---|---|
 * | 分块大小 | 64KB | 1MB |
 * | 文件大小 | 1GiB | 1TiB |
 * | 路径长度 | 1024B | 1024B |
 * | 单块失败重试 | 3 次 | 16 次 |
 *
 * 「重试 3 次」的含义：同一块第 1、2、3 次校验失败返回
 * `kChecksumMismatch`（调用方重发），第 4 次失败才返回 `kRetryExhausted`
 * ——即允许 3 次重发、共 4 次尝试。
 *
 * ## 帧格式（大端）
 *
 * | 类型 | 固定头 | 布局 |
 * |---|---|---|
 * | HEADER(1) | 60B | magic/ver/type + transfer_id u32 + size u64 + chunk_size u32 + chunk_count u32 + sha256[32] + path_len u16 + reserved u16 + path |
 * | CHUNK(2) | 56B | magic/ver/type + transfer_id u32 + index u32 + offset u64 + data_len u32 + sha256[32] + data |
 * | ACK(3) | 24B | magic/ver/type + transfer_id u32 + next_index u32 + next_offset u64 + reason u8 + reserved[3] |
 *
 * 所有解码路径都校验固定头、字段上界与**精确总长**；截断/超长帧一律拒绝。
 *
 * ## 依赖
 *
 * SHA-256 复用 `go2cpp::hash::Sha256`（零依赖的仓库内实现），不引入
 * OpenSSL 等外部库；文件 IO 用标准库 `std::fstream` / `std::filesystem`。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::bento {

// ---------------------------------------------------------------------------
// 协议常量与上限
// ---------------------------------------------------------------------------

/// 文件传输帧版本。
inline constexpr std::uint8_t kFileProtocolVersion = 1;
/// 文件传输帧 magic 首字节。
inline constexpr std::uint8_t kFileFrameMagic0 = 'B';
/// 文件传输帧 magic 次字节。
inline constexpr std::uint8_t kFileFrameMagic1 = 'F';
/// HEADER 帧类型。
inline constexpr std::uint8_t kFileFrameTypeHeader = 1;
/// CHUNK 帧类型。
inline constexpr std::uint8_t kFileFrameTypeChunk = 2;
/// ACK 帧类型。
inline constexpr std::uint8_t kFileFrameTypeAck = 3;
/// HEADER 帧固定头长度。
inline constexpr std::size_t kFileHeaderFrameHeaderBytes = 60;
/// CHUNK 帧固定头长度。
inline constexpr std::size_t kFileChunkFrameHeaderBytes = 56;
/// ACK 帧长度。
inline constexpr std::size_t kFileAckFrameBytes = 24;
/// 默认分块大小（64KB）。
inline constexpr std::size_t kFileChunkBytes = 64 * 1024;
/// 分块大小硬上限（1MB）。
inline constexpr std::size_t kFileMaxChunkBytes = 1024 * 1024;
/// 默认文件大小上限（1GiB）。
inline constexpr std::uint64_t kFileDefaultMaxBytes = 1ull << 30;
/// 文件大小硬上限（1TiB）。
inline constexpr std::uint64_t kFileMaxBytesCeiling = 1ull << 40;
/// 路径最大字节数。
inline constexpr std::size_t kFileMaxPathBytes = 1024;
/// 默认单块重试次数。
inline constexpr int kFileDefaultMaxRetries = 3;
/// 重试次数硬上限。
inline constexpr int kFileMaxRetriesCeiling = 16;
/// SHA-256 摘要字节数。
inline constexpr std::size_t kFileHashBytes = 32;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

/**
 * @brief 文件传输的状态码。
 *
 * @details `kDuplicateChunk` 是**幂等成功**（旧块重发，不推进 offset），
 *          调用方可以继续；其余非 `kOk` 值都需要重发或终止传输。
 */
enum class FileTransferStatus : std::uint8_t {
    kOk = 0,                ///< 成功/已推进。
    kInvalidArgument = 1,   ///< 参数或帧字段非法。
    kPathRejected = 2,      ///< 传输路径未通过沙箱校验。
    kTooLarge = 3,          ///< 文件超过大小上限。
    kIoError = 4,           ///< 本地文件读写失败。
    kChecksumMismatch = 5,  ///< 块/整文件摘要不一致（可重试）。
    kSequenceError = 6,     ///< 块序号/偏移不连续。
    kRetryExhausted = 7,    ///< 同一块失败次数超过重试上限。
    kDuplicateChunk = 8,    ///< 已写入过的旧块（幂等，不推进）。
    kNotReady = 9,          ///< 未 Open / 已关闭。
    kAlreadyCompleted = 10, ///< 传输已成功结束。
    kTargetExists = 11,     ///< 目标已存在且未允许覆盖。
    kCancelled = 12,        ///< 调用方已取消。
};

/// 状态转文本（诊断/日志用）。
const char* FileTransferStatusToString(FileTransferStatus status) noexcept;

/// SHA-256 摘要（32B）。
using FileHash = std::array<std::uint8_t, kFileHashBytes>;

/// 摘要转 64 位小写十六进制。
std::string FileHashHex(const FileHash& hash);

/// 计算一段字节的 SHA-256（空输入也有定义值）。
FileHash Sha256Of(const std::uint8_t* data, std::size_t size);

// ---------------------------------------------------------------------------
// 配置与消息
// ---------------------------------------------------------------------------

/** @brief 两端共用的传输配置（接收端的上限更严格，因为要落盘）。 */
struct FileTransferConfig final {
    /// 分块大小（1 .. `kFileMaxChunkBytes`）。
    std::size_t chunk_size{kFileChunkBytes};
    /// 允许的最大文件字节数（<= `kFileMaxBytesCeiling`）。
    std::uint64_t max_file_bytes{kFileDefaultMaxBytes};
    /// 单块校验失败允许的重试次数（0 .. `kFileMaxRetriesCeiling`）。
    int max_retries{kFileDefaultMaxRetries};
    /// 相对路径最大字节数（<= `kFileMaxPathBytes`）。
    std::size_t max_path_bytes{kFileMaxPathBytes};
    /// 目标文件已存在时是否允许覆盖（默认拒绝）。
    bool overwrite{false};
};

/** @brief 文件级描述（HEADER 帧的值对象）。 */
struct FileHeader final {
    /// 传输 id（由发送端分配，两端回显）。
    std::uint32_t transfer_id{0};
    /// 相对路径（由发送端给出，接收端做沙箱解析）。
    std::string path;
    /// 文件总字节数。
    std::uint64_t size{0};
    /// 分块大小。
    std::uint32_t chunk_size{0};
    /// 分块总数（`size == 0` 时为 0）。
    std::uint32_t chunk_count{0};
    /// 整文件 SHA-256。
    FileHash sha256{};
};

/** @brief 一个数据块（CHUNK 帧的值对象）。 */
struct FileChunk final {
    /// 传输 id。
    std::uint32_t transfer_id{0};
    /// 块下标（0 起、连续）。
    std::uint32_t index{0};
    /// 块在文件中的偏移（必须等于 `index * chunk_size`）。
    std::uint64_t offset{0};
    /// 块数据。
    std::vector<std::uint8_t> data;
    /// 块数据的 SHA-256。
    FileHash sha256{};
};

/** @brief ACK 的原因分类。 */
enum class FileAckReason : std::uint8_t {
    kContinue = 0,   ///< 正常推进（携带下一次期望的 index/offset）。
    kRetryChunk = 1, ///< 请求重发当前块（校验失败）。
    kCompleted = 2,  ///< 整文件已完成。
    kRejected = 3,   ///< 拒绝：路径/大小/配置非法。
    kCancelled = 4,  ///< 调用方取消。
};

/** @brief 接收端回执（ACK 帧的值对象）。 */
struct FileAck final {
    /// 传输 id。
    std::uint32_t transfer_id{0};
    /// 下一次期望的块下标。
    std::uint32_t next_index{0};
    /// 下一次期望的字节偏移。
    std::uint64_t next_offset{0};
    /// 回执原因。
    FileAckReason reason{FileAckReason::kContinue};
};

// ---------------------------------------------------------------------------
// 路径沙箱
// ---------------------------------------------------------------------------

/**
 * @brief 把发送端的相对路径解析成 root 下的绝对路径。
 * @param root 接收根目录（必须存在且是目录）。
 * @param relative_path 发送端给出的相对路径。
 * @param max_path_bytes 允许的最大路径字节数。
 * @param resolved 成功时写入解析后的绝对路径。
 * @return `kOk` 或 `kPathRejected`/`kIoError`/`kInvalidArgument`。
 */
FileTransferStatus ResolveTransferPath(const std::string& root,
                                      const std::string& relative_path,
                                      std::size_t max_path_bytes, std::string* resolved);

// ---------------------------------------------------------------------------
// 帧编解码
// ---------------------------------------------------------------------------

/// 编码 HEADER 帧；字段非法时返回空 vector。
std::vector<std::uint8_t> EncodeFileHeader(const FileHeader& header);

/// 解码 HEADER 帧；失败返回 false 并（可选）写 `error`。
bool DecodeFileHeader(const std::uint8_t* data, std::size_t size, FileHeader* out,
                      std::string* error = nullptr);

/// 编码 CHUNK 帧；字段非法时返回空 vector。
std::vector<std::uint8_t> EncodeFileChunk(const FileChunk& chunk);

/// 解码 CHUNK 帧；失败返回 false 并（可选）写 `error`。
bool DecodeFileChunk(const std::uint8_t* data, std::size_t size, FileChunk* out,
                     std::string* error = nullptr);

/// 编码 ACK 帧。
std::vector<std::uint8_t> EncodeFileAck(const FileAck& ack);

/// 解码 ACK 帧；失败返回 false 并（可选）写 `error`。
bool DecodeFileAck(const std::uint8_t* data, std::size_t size, FileAck* out,
                   std::string* error = nullptr);

// ---------------------------------------------------------------------------
// 发送端
// ---------------------------------------------------------------------------

/**
 * @brief 文件发送端：Open 时算整文件摘要，按需读取并生成 CHUNK 帧。
 *
 * @details 只持有源文件的读句柄，不把整个文件读进内存；`BuildChunkFrame`
 *          可以按任意顺序调用（重复发送/断点续传都靠它）。非线程安全：
 *          同一个发送端只允许一个线程使用。
 */
class FileSender final {
public:
    FileSender();
    ~FileSender();

    FileSender(const FileSender&) = delete;
    FileSender& operator=(const FileSender&) = delete;

    /**
     * @brief 打开源文件并计算整文件 SHA-256。
     * @param source_path 本地源文件路径。
     * @param transfer_path 传输给对端的相对路径（接收端会做沙箱校验）。
     * @param transfer_id 传输 id。
     * @param config 传输配置。
     */
    FileTransferStatus Open(const std::string& source_path, const std::string& transfer_path,
                            std::uint32_t transfer_id,
                            const FileTransferConfig& config = FileTransferConfig{});

    /// 当前文件描述；未打开时返回全零描述。
    const FileHeader& header() const noexcept;
    /// 分块总数。
    std::uint32_t chunk_count() const noexcept;
    /// 下一次 `BuildNextChunkFrame` 会发送的块下标。
    std::uint32_t next_index() const noexcept;
    /// 下一次发送的字节偏移。
    std::uint64_t next_offset() const noexcept;
    /// 是否已打开。
    bool opened() const noexcept;

    /// 断点续传：把发送位置对齐到 `offset` 所在的块（offset 必须 <= size）。
    FileTransferStatus Seek(std::uint64_t offset);

    /// 回到文件开头。
    void Rewind();

    /// 读取并编码指定块；失败返回空帧与错误码。
    FileTransferStatus BuildChunkFrame(std::uint32_t index, std::vector<std::uint8_t>* frame);

    /// 编码 `next_index()` 指向的块并推进；块已发完返回 `kAlreadyCompleted`。
    FileTransferStatus BuildNextChunkFrame(std::vector<std::uint8_t>* frame);

    /// 关闭源文件并重置状态。
    void Close();

private:
    struct Impl;
    /// PIMPL：隐藏文件流与配置，避免头文件拖入 <fstream>。
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// 接收端
// ---------------------------------------------------------------------------

/**
 * @brief 文件接收端：把 CHUNK 顺序写入 `<target>.part`，Finish 校验后重命名。
 *
 * @details 非线程安全；同一时刻只处理一个传输。析构只关闭文件、**保留**
 *          `.part`（支持断点续传），要清理请显式 `Cancel()`。
 */
class FileReceiver final {
public:
    FileReceiver();
    ~FileReceiver();

    FileReceiver(const FileReceiver&) = delete;
    FileReceiver& operator=(const FileReceiver&) = delete;

    /**
     * @brief 校验 HEADER 并打开目标（可能是续传）。
     * @param header 发送端的文件描述。
     * @param root_dir 接收根目录（必须存在）。
     * @param config 传输配置。
     */
    FileTransferStatus Open(const FileHeader& header, const std::string& root_dir,
                            const FileTransferConfig& config = FileTransferConfig{});

    /// 当前文件描述；未打开时返回全零描述。
    const FileHeader& header() const noexcept;
    /// 目标文件绝对路径。
    std::string target_path() const;
    /// 临时文件（`.part`）绝对路径。
    std::string temp_path() const;
    /// 下一次期望的块下标。
    std::uint32_t next_index() const noexcept;
    /// 下一次期望的字节偏移。
    std::uint64_t next_offset() const noexcept;
    /// Open 时从已有 `.part` 恢复的偏移（0 表示没有续传）。
    std::uint64_t resume_offset() const noexcept;
    /// 累计重试次数（每块校验失败 +1）。
    int retries_used() const noexcept;
    /// 当前块的连续失败次数（成功写入一块后归零）。
    int chunk_failures() const noexcept;
    /// 是否已打开。
    bool opened() const noexcept;
    /// 是否已完成（Finish 成功）。
    bool completed() const noexcept;

    /// 应用一个数据块（校验 index/offset/长度/摘要，按序写入）。
    FileTransferStatus ApplyChunk(const FileChunk& chunk);

    /// 解码并应用一个 CHUNK 帧。
    FileTransferStatus ApplyChunkFrame(const std::uint8_t* data, std::size_t size,
                                       std::string* error = nullptr);

    /// 校验整文件并原子重命名为目标文件；失败时删除 `.part`。
    FileTransferStatus Finish();

    /// 取消：关闭并删除 `.part`，重置状态。
    void Cancel();

    /// 生成当前状态对应的 ACK。
    FileAck MakeAck(FileAckReason reason) const;

private:
    struct Impl;
    /// PIMPL：隐藏文件流、路径与重试计数。
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::bento
