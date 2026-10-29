#pragma once

/**
 * @file zip.hpp
 * @brief MOD-31：自研 store-only ZIP 归档（写入 + 读取），零第三方依赖。
 * @details 备份包只需要「把一棵目录树原样装进一个文件」，不需要压缩。
 *          store-only（method 0）让格式、CRC 与偏移都能用几十行标准库代码
 *          完成，也避免为 zlib/libzip 增加构建依赖。生成的归档与
 *          `unzip`/`zipinfo` 完全兼容；读取器同样只接受 method 0，
 *          遇到压缩条目明确返回 `kUnsupportedCompression`，不静默跳过。
 *
 * ## 格式
 *
 * 每个条目依次是 `本地文件头 + 原始数据`，全部条目之后是
 * `中央目录 + EOCD`。本实现使用固定 30B 本地头、46B 中央目录项，
 * 不写 extra field / 注释 / 数据描述符。所有多字节字段按 ZIP 规范
 * 使用**小端**，CRC 为 IEEE CRC-32（复用 `go2cpp::hash`）。
 *
 * | 结构 | 签名 | 长度 |
 * |---|---|---|
 * | 本地文件头 | `PK\x03\x04` | 30 + name |
 * | 中央目录项 | `PK\x01\x02` | 46 + name |
 * | EOCD | `PK\x05\x06` | 22 |
 *
 * ## 上限（防 zip 炸弹 / 防 4GiB 溢出）
 *
 * ZIP32 的偏移与大小都是 32 位；本实现不写 ZIP64。EOCD 的条目计数字段是
 * u16，而 `0xFFFF` 是 ZIP64 的哨兵值（读取端见到它必须按 ZIP64 解析），
 * 因此**可安全表示的条目数上限是 65534**：写 65535 条会生成一个自己都读
 * 不回来的归档。归档总字节必须 `< 4GiB`。`ZipLimits` 的默认值更保守
 * （16384 条目 / 单文件 1GiB / 总量 2GiB），调用方可以按场景收紧，
 * 但不能突破硬上限。写入与读取**都**检查上限：读取端先看中央目录声明的
 * 大小，再决定是否解压，避免「小包声明 4GiB」的炸弹。
 *
 * ## 路径与符号链接
 *
 * 归档内名字必须是相对路径：拒绝绝对路径、`..` 段、反斜杠、冒号与
 * 控制字符。`AddDirectory` 递归时**跳过符号链接**（目录链接不跟随，
 * 防止逃逸与环）并计入 `symlinks_skipped`；FIFO/设备文件等特殊文件
 * 同样跳过并计入 `special_skipped`。解压端重新校验每个名字并检查
 * 解析后的路径仍在目标目录内（Zip Slip 防护）。
 *
 * ## 线程模型
 *
 * `ZipWriter`、`ListZip`、`ExtractZip` 都是阻塞式、非线程安全的值对象：
 * 一个实例只允许一个线程使用，不同实例可以并行（各自独立的文件句柄）。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace go2cpp::bento::app {

// ---------------------------------------------------------------------------
// 常量与状态
// ---------------------------------------------------------------------------

/// 默认条目上限（文件 + 目录）。
inline constexpr std::size_t kZipDefaultMaxEntries = 16384;
/// 条目数硬上限（EOCD 的 u16 计数字段；0xFFFF 是 ZIP64 哨兵，故为 65534）。
inline constexpr std::size_t kZipMaxEntriesCeiling = 65534;
/// 默认单条目上限（1GiB）。
inline constexpr std::uint64_t kZipDefaultMaxEntryBytes = 1ull << 30;
/// 默认归档总量上限（2GiB）。
inline constexpr std::uint64_t kZipDefaultMaxTotalBytes = 2ull << 30;
/// ZIP32 可表示的最大字节数（4GiB - 1）。
inline constexpr std::uint64_t kZipMaxBytesCeiling = 0xffffffffull;

/**
 * @brief ZIP 操作状态码。
 *
 * @details 所有非 `kOk` 值都会让写入器进入失败态（poison），调用方应
 *          `Abort()` 清理半成品；读取失败不会修改已有文件（损坏的单个
 *          输出文件会被删除）。
 */
enum class ZipStatus : std::uint8_t {
    kOk = 0,                     ///< 成功。
    kInvalidArgument = 1,        ///< 参数非法（空名字、越界配置等）。
    kInputNotFound = 2,          ///< 输入路径不存在或不可读。
    kIoError = 3,                ///< 本地文件读写/seek 失败。
    kTooManyEntries = 4,         ///< 条目数超过上限。
    kEntryTooLarge = 5,          ///< 单条目超过上限。
    kArchiveTooLarge = 6,        ///< 归档总量超过上限或 4GiB 边界。
    kUnsupportedCompression = 7, ///< 条目 method 非 0（本实现只支持 store）。
    kCorruptArchive = 8,         ///< 签名/长度/CRC 校验失败。
    kPathRejected = 9,           ///< 归档内名字未通过相对路径校验。
    kNotOpen = 10,               ///< 写入器尚未 Open。
    kAlreadyClosed = 11,         ///< 写入器已经 Close。
};

/// 状态转文本（日志/诊断用）。
const char* ZipStatusToString(ZipStatus status) noexcept;

/** @brief 写入与读取共用的上限。 */
struct ZipLimits final {
    /// 条目总数上限（文件 + 目录），1 .. `kZipMaxEntriesCeiling`。
    std::size_t max_entries{kZipDefaultMaxEntries};
    /// 单个条目上限（未压缩字节），1 .. `kZipMaxBytesCeiling`。
    std::uint64_t max_entry_bytes{kZipDefaultMaxEntryBytes};
    /// 归档内所有条目未压缩字节之和上限，1 .. `kZipMaxBytesCeiling`。
    std::uint64_t max_total_bytes{kZipDefaultMaxTotalBytes};
};

/** @brief 写入/读取的统计。 */
struct ZipStats final {
    std::size_t entries{0};          ///< 已处理条目总数（文件 + 目录）。
    std::size_t files{0};            ///< 普通文件条目数。
    std::size_t directories{0};      ///< 目录条目数。
    std::size_t symlinks_skipped{0}; ///< 递归时跳过的符号链接数。
    std::size_t special_skipped{0};  ///< 递归时跳过的特殊文件数。
    std::uint64_t input_bytes{0};    ///< 文件条目未压缩字节之和。
    std::uint64_t archive_bytes{0};  ///< 归档文件总字节数。
};

/** @brief 写入/解压结果。 */
struct ZipResult final {
    ZipStatus status{ZipStatus::kOk}; ///< 状态。
    ZipStats stats;                   ///< 统计（失败时保留已完成部分）。
    std::string error;                ///< 人类可读的失败原因。

    bool ok() const noexcept { return status == ZipStatus::kOk; }
};

/** @brief 中央目录中的单个条目描述。 */
struct ZipEntryInfo final {
    std::string name;       ///< 原始条目名（目录以 `/` 结尾）。
    std::uint64_t size{0};  ///< 未压缩字节数。
    std::uint32_t crc32{0}; ///< IEEE CRC-32。
    bool is_directory{false}; ///< 名字以 `/` 结尾或是目录条目。
};

/** @brief `ListZip` 的结果。 */
struct ZipListing final {
    ZipStatus status{ZipStatus::kOk};  ///< 状态。
    std::vector<ZipEntryInfo> entries; ///< 按中央目录顺序排列的条目。
    ZipStats stats;                    ///< 统计。
    std::string error;                 ///< 失败原因。

    bool ok() const noexcept { return status == ZipStatus::kOk; }
};

// ---------------------------------------------------------------------------
// 写入器
// ---------------------------------------------------------------------------

/**
 * @brief store-only ZIP 写入器。
 *
 * @details 典型用法：
 * @code
 * ZipWriter writer;
 * if (!writer.Open("/tmp/a.zip").ok()) { ... }
 * writer.AddFile("/data/a.txt", "a.txt");
 * writer.AddDirectory("/data/photos", "photos");
 * if (!writer.Close().ok()) { ... }   // 失败时自动删除半成品
 * @endcode
 *
 * 每次写入前检查上限；一旦某个 Add 失败，实例进入失败态，后续操作只
 * 返回错误，必须 `Abort()`（析构也会自动清理）。`Close` 写中央目录与
 * EOCD 后关闭文件；失败时删除输出文件。调用方若需要「旧包永远可用」
 * 请自行写临时名后 rename（`Backup` 就是这样做的）。
 */
class ZipWriter final {
public:
    ZipWriter();
    ~ZipWriter();

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    /**
     * @brief 创建/截断输出文件。
     * @param output_path 输出 zip 路径；父目录不存在时自动创建。
     * @param limits 条目数与大小上限。
     */
    ZipStatus Open(const std::string& output_path, const ZipLimits& limits = ZipLimits{});

    /**
     * @brief 把一个普通文件写成条目。
     * @param source_path 本地源文件（必须是普通文件，不跟随符号链接）。
     * @param entry_name 归档内相对名字（不能以 `/` 开头）。
     */
    ZipStatus AddFile(const std::string& source_path, const std::string& entry_name);

    /**
     * @brief 把一段内存写成条目。
     * @param entry_name 归档内相对名字。
     * @param data 数据指针；`size == 0` 时允许为 nullptr。
     * @param size 字节数。
     */
    ZipStatus AddBytes(const std::string& entry_name, const std::uint8_t* data,
                       std::size_t size);

    /**
     * @brief 递归添加目录。
     * @param source_dir 本地目录。
     * @param entry_prefix 归档内目录名（不含末尾 `/`）；空表示不写根条目，
     *        只写内容。非空时先写 `entry_prefix/` 目录条目。
     *
     * @details 条目按路径排序后写入，保证同样的输入产生稳定的顺序；跳过
     *          符号链接与特殊文件。已经通过 `AddExclusion` 登记的输出文件
     *          不会被打进归档。
     */
    ZipStatus AddDirectory(const std::string& source_dir, const std::string& entry_prefix = {});

    /**
     * @brief 登记一个不参与递归的本地路径（通常是输出 zip 自身）。
     *
     * @details 递归时按绝对路径比较；用于防止「备份目录包含输出包」导致
     *          归档把自己打包进去。
     */
    ZipStatus AddExclusion(const std::string& path);

    /// 写中央目录与 EOCD；成功后实例关闭。失败时删除输出文件。
    ZipStatus Close();

    /// 关闭并删除输出文件（幂等）。析构时未 Close 会自动调用。
    void Abort() noexcept;

    /// 是否已 Open 且未 Close/Abort。
    bool opened() const noexcept;
    /// 是否已成功 Close。
    bool closed() const noexcept;
    /// 累计统计。
    const ZipStats& stats() const noexcept;
    /// 当前生效的上限。
    const ZipLimits& limits() const noexcept;

private:
    struct Impl;
    /// PIMPL：隐藏 ofstream 与条目表，保持头文件轻量。
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// 读取器
// ---------------------------------------------------------------------------

/**
 * @brief 只读中央目录，返回条目列表。
 * @param zip_path 输入 zip。
 * @param limits 条目数与大小上限（用于拒绝异常归档）。
 *
 * @details 不读取条目数据，因此不会触发解压；仍会校验中央目录偏移与
 *          每个条目的本地头签名。
 */
ZipListing ListZip(const std::string& zip_path, const ZipLimits& limits = ZipLimits{});

/**
 * @brief 把 store-only zip 解压到目录（覆盖同名文件）。
 * @param zip_path 输入 zip。
 * @param output_dir 输出根目录（不存在时创建）。
 * @param limits 条目数/单条/总量上限。
 *
 * @details 名字经过 Zip Slip 校验；每个文件写入后校验字节数与 CRC-32，
 *          不一致时删除该文件并返回 `kCorruptArchive`。目录条目会创建
 *          空目录。已成功写出的其它文件保留（调用方负责清理临时目录）。
 */
ZipResult ExtractZip(const std::string& zip_path, const std::string& output_dir,
                     const ZipLimits& limits = ZipLimits{});

}  // namespace go2cpp::bento::app
