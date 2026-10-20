#pragma once

/**
 * @file split.hpp
 * @brief 长记录的前缀分片：按检查点切出一组**可独立重放**的记录。
 *
 * ## 语义
 *
 * `part0001` = `[文件头, kMeta, 事件 0..N1, kNote(检查点), kEnd]`；
 * `part0002` = 前一片的全部事件 + 下一段，依此类推；最后一片就是完整记录。
 *
 * 这样每个分片都能**单独** `replay`（不需要前序状态），适合：
 * - 二分定位「出事点」：先放最后一个分片，若 stuck 再往前找；
 * - 只重放到检查点：`replay play part0003` 或 `replay play file --until=SEQ`。
 *
 * 代价是存储放大 O(分片数 × 前缀大小)。文档 §限制 明确说明；如果不接受，
 * 用 `play --until` 在完整文件上截断即可（不产生额外文件）。
 *
 * ## 保真
 *
 * 事件原样复制（seq / ts / vtime 不变），因此分片与源文件的检查点可以直接
 * 用 `seq` 对照。每个分片重新计算整文件滚动 CRC。
 */

#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::replay {

/// 分片选项。
struct SplitOptions {
    std::string out_dir;      ///< 输出目录；空表示源文件所在目录
    std::string prefix;       ///< 文件名前缀；空表示源文件名（去扩展名）
    std::uint32_t every_events{0};  ///< 每 N 条事件一个检查点（0=关闭）
    std::uint64_t every_ns{0};      ///< 每 M 纳秒虚拟时间一个检查点（0=关闭）
};

/// 一个分片的元信息。
struct SplitPart {
    std::string path;
    std::uint64_t first_seq{0};
    std::uint64_t last_seq{0};
    std::uint64_t events{0};
    std::uint64_t vtime_ns{0};
};

/// 分片结果。
struct SplitResult {
    bool ok{false};
    std::string error;
    std::uint64_t source_events{0};
    std::vector<SplitPart> parts;
};

/**
 * @brief 把 `path` 按检查点切成前缀分片。
 *
 * `every_events` 与 `every_ns` 至少一个非 0；两者同时给出时任一命中即切。
 * 若记录比阈值短，只产生一个分片（完整记录）。
 */
SplitResult SplitRecording(const std::string& path, const SplitOptions& options);

}  // namespace go2cpp::replay
