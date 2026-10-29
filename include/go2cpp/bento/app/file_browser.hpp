#pragma once

/**
 * @file file_browser.hpp
 * @brief MOD-30：受限根目录的文件浏览/下载/上传（List/Get/Put）。
 * @details 本模块把一个**根目录**当作沙箱，所有相对路径都必须解析到根目录
 *          之下；它不负责网络传输，只负责本地文件系统语义，因此可以同时
 *          服务于「控制面浏览对端目录」与「本地任务下载/上传文件」两类场景。
 *
 *          ```
 *          root/
 *            ├─ docs/            List("docs") → [a.txt, b.txt]
 *            ├─ a.txt            Get("a.txt", "/tmp/a.txt") → 本地副本
 *            └─ upload/          Put("upload/c.bin", "/tmp/c.bin") → 落到 root 内
 *          ```
 *
 * ## 路径沙箱（默认拒绝）
 *
 * 相对路径必须满足：
 * - 非空（`List` 允许空串表示根目录本身）；
 * - 不以 `/` 开头；不含 `\`、`:`、NUL/控制字符；长度 <= `max_path_bytes`；
 * - 按 `/` 分段后每段都非空，且不是 `.` 或 `..`（因此 `a/../b` 也被拒绝，
 *   而不是规范化后放行——拒绝比修复更容易审计）；
 * - 解析后（`weakly_canonical`，含已存在部分的符号链接）仍在根目录之下；
 * - 路径上任何已存在的符号链接默认直接拒绝（`follow_symlinks=false`）。
 *   打开文件时额外带 `O_NOFOLLOW`，避免「检查后替换成链接」的 TOCTOU。
 *
 * 根目录本身在构造时规范化（`weakly_canonical`），因此根目录是符号链接时
 * 以解析后的真实路径为边界。**构造后**根目录仍会在每次操作前用 `lstat` 复核：
 *
 * - 被删除 → 全部操作 `kNotFound`，`Put` 不会把根目录重新创建出来；
 * - 被替换成符号链接/非目录 → `kPathRejected`，避免 `List("")` 直接把根路径
 *   交给目录遍历而列出外部内容（fail-closed）。
 *
 * 默认（`follow_symlinks=false`）的路径解析是「逐段 `lstat` + 词法拼接」：
 * 只要所有已存在段都不是符号链接、根也不是符号链接，词法拼接就等于真实路径，
 * 不再调用 `weakly_canonical`（省掉 per-component 的第二次遍历）；只有开启
 * 跟随并真的遇到符号链接时才做真实路径判定。
 *
 * ## 目录项语义
 *
 * `List` 返回 `name`（不含父路径）、`size`（普通文件为字节数；目录/链接为
 * `lstat` 的 `st_size`）、`mtime_ms`（Unix epoch 毫秒）与 `type`。条目按
 * `name` 字节序升序排列，便于调用方稳定展示与对拍；`include_hidden=false`
 * 时跳过以 `.` 开头的条目。
 *
 * ## 上传/下载的原子性
 *
 * `Get`/`Put` 都先写同目录下的临时文件（`*` 前缀 + pid + 序号，
 * `O_CREAT|O_EXCL|O_NOFOLLOW`），复制完整后再发布。中途失败会清理临时
 * 文件，不会留下半截目标；临时名连续被占用（崩溃残留/外部干扰）时会换序号
 * 重试，不会把「临时名已存在」误报成「目标已存在」。
 *
 * `overwrite=false`（默认）时发布优先用 Linux `renameat2(RENAME_NOREPLACE)`，
 * 一次性完成「目标不存在才发布」，不依赖文件系统硬链接支持；系统调用不可用
 * （旧内核/文件系统不支持 flags）时退化为 `link()` + `unlink()`——目标已存在
 * 时内核以 `EEXIST` 拒绝，因此「先检查再改名」的 TOCTOU 窗口不存在，并发
 * 创建的目标不会被本次写入覆盖（`kAlreadyExists`）。`overwrite=true` 时用
 * `rename()` 原子替换。
 *
 * ## 错误分类
 *
 * 与 `FetchStatus` 分开：文件浏览不涉及网络，错误只区分参数/沙箱/不存在/
 * 类型不符/过大/IO/权限/已存在/条目过多。`kNotRegular` 同时覆盖「List 的
 * 目标不是目录」与「Get/Put 的源不是普通文件」。`kPathRejected` 还覆盖
 * `ENAMETOOLONG`/`ELOOP`（超长名、符号链接层数过多），保证同一路径经
 * `List` 与 `Stat` 得到一致分类。`kNotFound` 覆盖根目录在构造后被删除。
 * `local_path` 含 NUL 时返回 `kInvalidArgument`（否则底层 `open()` 会静默
 * 截断到 NUL 前，写到意料之外的路径）。
 *
 * @note 同步阻塞；单次调用只做一次目录遍历或一次文件复制，不提供流式接口。
 * @note 不跟随符号链接是默认值；打开 `follow_symlinks` 后仍以规范化后的
 *       真实路径是否在根目录内为最终判据。检查与打开之间仍有文件系统级别的
 *       TOCTOU 窗口（默认模式用 `O_NOFOLLOW` 覆盖最后一段），需要更强保证
 *       的调用方应使用 `openat2(RESOLVE_BENEATH)` 之类的手段。
 * @note `const` 方法线程安全（无共享可变状态；临时序号用原子变量）。并发
 *       `Put` 同一目标且 `overwrite=false` 时恰好一个返回 `kOk`，其余
 *       `kAlreadyExists`。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::bento::app {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/// 默认单文件大小上限（1 GiB）。
inline constexpr std::uint64_t kFileBrowserDefaultMaxBytes = 1ull << 30;
/// 单文件大小硬上限（1 TiB）。
inline constexpr std::uint64_t kFileBrowserMaxBytesCeiling = 1ull << 40;
/// 默认相对路径长度上限（字节）。
inline constexpr std::size_t kFileBrowserDefaultMaxPathBytes = 1024;
/// 相对路径长度硬上限（字节）。
inline constexpr std::size_t kFileBrowserMaxPathBytes = 4096;
/// 默认单次列目录的条目上限。
inline constexpr std::size_t kFileBrowserDefaultMaxEntries = 100000;
/// 单次列目录的条目数硬上限（与 `FileBrowser` 构造校验共用）。
inline constexpr std::size_t kFileBrowserMaxEntriesCeiling = 10 * 1000 * 1000;

// ---------------------------------------------------------------------------
// 状态与数据结构
// ---------------------------------------------------------------------------

/// 目录项类型（与 `struct stat` 的互斥类别对应）。
enum class FileEntryType : std::uint8_t {
    kFile = 0,       ///< 普通文件。
    kDirectory = 1,  ///< 目录。
    kSymlink = 2,    ///< 符号链接（`List`/`Stat` 默认不跟随）。
    kOther = 3,      ///< FIFO、socket、设备等。
};

/// 目录项类型转文本。
const char* FileEntryTypeToString(FileEntryType type) noexcept;

/// 文件浏览操作的状态码。
enum class FileBrowserStatus : std::uint8_t {
    kOk = 0,                ///< 成功。
    kInvalidArgument = 1,   ///< 参数为空/越界（entries 为空、路径超长等）。
    kPathRejected = 2,      ///< 相对路径未通过沙箱校验。
    kNotFound = 3,          ///< 目标不存在。
    kNotRegular = 4,        ///< 类型不符（List 目标非目录、Get/Put 源非文件）。
    kTooLarge = 5,          ///< 文件超过大小上限。
    kIoError = 6,           ///< 本地 IO/rename 失败。
    kPermissionDenied = 7,  ///< EACCES/EPERM。
    kAlreadyExists = 8,     ///< 目标已存在且未允许覆盖。
    kTooManyEntries = 9,    ///< 目录条目超过 max_entries。
};

/// 状态转文本（诊断/日志用，英文短名）。
const char* FileBrowserStatusToString(FileBrowserStatus status) noexcept;

/** @brief 一个目录项/文件描述。 */
struct FileEntry final {
    /// 名称（`List` 为子项名，`Stat` 为最后一段）。
    std::string name;
    /// 字节数（普通文件为内容长度；其他类型为 `lstat` 的 `st_size`）。
    std::uint64_t size{0};
    /// 最后修改时间（Unix epoch 毫秒；未知为 0）。
    std::int64_t mtime_ms{0};
    /// 类型。
    FileEntryType type{FileEntryType::kOther};
};

/** @brief 文件浏览选项；默认值即安全默认。 */
struct FileBrowserOptions final {
    /// 单文件大小上限（1 .. `kFileBrowserMaxBytesCeiling`）。
    std::uint64_t max_file_bytes{kFileBrowserDefaultMaxBytes};
    /// 相对路径长度上限（1 .. `kFileBrowserMaxPathBytes`）。
    std::size_t max_path_bytes{kFileBrowserDefaultMaxPathBytes};
    /// 单次 List 的条目上限（1 .. `kFileBrowserMaxEntriesCeiling`）。
    std::size_t max_entries{kFileBrowserDefaultMaxEntries};
    /// 目标已存在时是否允许覆盖（默认拒绝）。
    bool overwrite{false};
    /// 是否跟随路径上的符号链接（默认拒绝，且以真实路径为最终判据）。
    bool follow_symlinks{false};
    /// 是否包含以 `.` 开头的隐藏条目（默认包含）。
    bool include_hidden{true};
};

// ---------------------------------------------------------------------------
// 类接口
// ---------------------------------------------------------------------------

/**
 * @brief 绑定一个根目录的文件浏览器。
 *
 * @details 构造时规范化根目录；根目录不存在、不是目录或无法解析时对象处于
 *          无效状态（`valid()==false`），后续所有操作返回 `kPathRejected`。
 *          这样调用方不必在每次操作前重复检查根目录。
 */
class FileBrowser final {
public:
    /**
     * @param root 根目录（绝对或相对路径均可，构造时规范化）。
     * @param options 选项。
     */
    explicit FileBrowser(std::string root, FileBrowserOptions options = FileBrowserOptions{});

    /// 根目录是否有效。
    bool valid() const noexcept { return m_valid; }

    /// 规范化后的根目录；无效时为空。
    const std::string& root() const noexcept { return m_root; }

    /// 当前选项。
    const FileBrowserOptions& options() const noexcept { return m_options; }

    /**
     * @brief 列出 `rel` 目录下的条目。
     * @param rel 相对路径；空串表示根目录本身。
     * @param entries 输出（成功时被清空并填充；允许为空指针时返回参数错误）。
     *                返回 `kTooManyEntries` 时保留已扫描到的前 `max_entries` 项。
     * @return 状态。
     */
    FileBrowserStatus List(std::string_view rel, std::vector<FileEntry>* entries) const;

    /**
     * @brief 把 `rel` 指向的普通文件复制到本地路径 `local_path`。
     * @param rel 相对路径（不能为空）。
     * @param local_path 本地目标文件（父目录必须存在；默认不覆盖；含 NUL 返回
     *                   `kInvalidArgument`）。
     * @return 状态。
     */
    FileBrowserStatus Get(std::string_view rel, std::string_view local_path) const;

    /**
     * @brief 把本地路径 `local_path` 的普通文件复制到 `rel`。
     * @param rel 相对路径（不能为空；父目录不存在时自动创建）。
     * @param local_path 本地源文件（含 NUL 返回 `kInvalidArgument`）。
     * @return 状态。
     */
    FileBrowserStatus Put(std::string_view rel, std::string_view local_path) const;

    /**
     * @brief 查看 `rel` 的元信息（不跟随符号链接，空串表示根目录）。
     * @param rel 相对路径。
     * @param entry 输出。
     * @return 状态。
     */
    FileBrowserStatus Stat(std::string_view rel, FileEntry* entry) const;

private:
    /// 解析相对路径为根目录内的绝对路径（含沙箱与符号链接检查）。
    FileBrowserStatus Resolve(std::string_view rel, bool allow_root,
                              std::string* resolved) const;

    std::string m_root;                ///< 规范化后的根目录；无效时为空
    FileBrowserOptions m_options;      ///< 当前选项
    bool m_valid{false};               ///< 根目录是否可用
};

// ---------------------------------------------------------------------------
// 便捷自由函数（每个调用构造一次 FileBrowser）
// ---------------------------------------------------------------------------

/**
 * @brief 列出 `root` 下 `rel` 目录的条目。
 * @see FileBrowser::List
 */
FileBrowserStatus List(const std::string& root, std::string_view rel,
                       std::vector<FileEntry>* entries,
                       const FileBrowserOptions& options = FileBrowserOptions{});

/**
 * @brief 从 `root` 下载 `rel` 到 `local_path`。
 * @see FileBrowser::Get
 */
FileBrowserStatus Get(const std::string& root, std::string_view rel,
                      const std::string& local_path,
                      const FileBrowserOptions& options = FileBrowserOptions{});

/**
 * @brief 把 `local_path` 上传到 `root` 下的 `rel`。
 * @see FileBrowser::Put
 */
FileBrowserStatus Put(const std::string& root, std::string_view rel,
                      const std::string& local_path,
                      const FileBrowserOptions& options = FileBrowserOptions{});

}  // namespace go2cpp::bento::app
