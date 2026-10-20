#pragma once

/**
 * @file formats.hpp
 * @brief 文件探测、整文件读取与原子写入：CLI 与「文件类」子命令的唯一 IO 层。
 *
 * @details 三个下游命令（`replay play` / `migrate restore` / `debug`）都需要回答
 *          同一个问题：**这个文件到底是什么格式**。探测集中在这里，判定规则
 *          集中在 `ProbeFile`，与后端对齐时只改一处。
 *
 * ## 当前识别的格式（已按 2026-10-20 交付物对齐）
 *
 * | 格式 | 判定依据 | 归属 |
 * |---|---|---|
 * | 迁移快照（二进制） | 前 8 字节 `G2CMIG02`（v2，符号化） | `tools/migration/include/migration/format.hpp` |
 * | 重放记录（二进制） | 前 8 字节 `G2CRPLY\0` | `tools/replay/include/replay/format.hpp` |
 * | JSON | 去 BOM 后首字符 `{` / `[`（且不是 ini 节名） | DebugServer `JSON` payload、通用 dump |
 * | go2cpp-trace 文本 | 首行 `# go2cpp-trace` / `# go2cpp-replay` | `docs/trace_format.md` |
 * | ini | 前 32 行内出现 `[section]` 行 | 进程内 dump 的 ini 渲染 |
 *
 * ## 边界与陷阱
 *
 * - 探测**只读前 4 KiB**，不解析语义；JSON 语法由调用方用 nlohmann 校验，
 *   这样「格式像 JSON 但被截断」能给出解析器自己的行列号。
 * - 空文件判为格式错误（`kFormat`），不是 IO 错误：它确实存在，只是没有内容。
 * - 非普通文件（目录、FIFO、设备）一律拒绝，避免 `read` 阻塞在管道上。
 * - `WriteTextFileAtomic` 先写同目录临时文件再 `rename`，失败不会留下半截文件；
 *   但**不**为已存在的目标做备份 —— 覆盖语义由调用方在文档里说明。
 */

#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::cli {

/// 迁移快照魔数（8 字节，`migration::kMagic`；v1 的 `G2CMIG01` 已废弃）。
inline constexpr char kMigrationMagic[8] = {'G', '2', 'C', 'M', 'I', 'G', '0', '2'};
/// 重放记录魔数（8 字节，含结尾 NUL，`replay::kMagic`）。
inline constexpr char kReplayMagic[8] = {'G', '2', 'C', 'R', 'P', 'L', 'Y', '\0'};

/// 探测得到的文件格式。
enum class FileFormat {
    kUnknown,         ///< 无法识别。
    kJson,            ///< JSON 文本（对象或数组）。
    kTraceText,       ///< `# go2cpp-trace` / `# go2cpp-replay` 文本记录。
    kMigrationBinary, ///< `tools/migration` v2 的 `G2CMIG02` 快照。
    kReplayBinary,    ///< `tools/replay` 的 `G2CRPLY` 记录。
    kIniText,         ///< ini 格式（`[section]` + `key = value`）。
};

/// 文件探测结果。
struct FileProbe {
    std::string path;                         ///< 调用方传入的路径（原样）。
    std::uint64_t size{0};                    ///< 文件字节数。
    FileFormat format{FileFormat::kUnknown};  ///< 识别结果。
    std::string detail;                       ///< 人类可读的判定依据（用于错误与摘要）。
    std::string head;                         ///< 前 4 KiB 原始内容（已去 BOM）。
};

/**
 * @brief 打开并探测文件。
 *
 * @param path 文件路径。
 * @param max_bytes 允许的最大字节数；超过即报错，避免误读超大文件。
 * @return 探测结果。
 * @throws CliError `kFailure`：不存在、无权限、不是普通文件、过大；
 *                  `kFormat`：空文件。
 */
FileProbe ProbeFile(const std::string& path,
                    std::uint64_t max_bytes = 64ULL * 1024ULL * 1024ULL);

/**
 * @brief 读取整个文本文件。
 * @param path 路径。
 * @param max_bytes 上限（与 `ProbeFile` 保持一致）。
 * @param size_out 非空时写入实际字节数。
 * @return 文件内容。
 * @throws CliError `kFailure`：IO 失败或超过上限。
 */
std::string ReadTextFile(const std::string& path,
                         std::uint64_t max_bytes,
                         std::uint64_t* size_out = nullptr);

/**
 * @brief 读取二进制文件的前 `bytes` 个字节（用于读取格式头）。
 * @throws CliError `kFailure`：IO 失败或文件短于请求长度。
 */
std::string ReadFilePrefix(const std::string& path, std::size_t bytes);

/**
 * @brief 原子写入文本文件（同目录临时文件 + rename）。
 * @throws CliError `kFailure`：目录不可写、磁盘满、rename 失败。
 */
void WriteTextFileAtomic(const std::string& path, std::string_view data);

/// 格式的稳定名称（用于摘要与 `--json` 输出，不随语言变化）。
const char* FileFormatName(FileFormat format);

/// 从二进制头解析重放记录版本（`kReplayBinary` 专用；失败返回 0）。
std::uint16_t ReplayFormatVersion(const std::string& head);

/// 从二进制头解析迁移快照版本（`kMigrationBinary` 专用；非 v2 魔数返回 0）。
std::uint32_t MigrationFormatVersion(const std::string& head);

}  // namespace go2cpp::cli
