#pragma once

/**
 * @file reader.hpp
 * @brief 持久化原型读取端：单文件顺序解码 + 段文件按 seq/时间范围定位。
 *
 * 读取路径：
 *   - 单文件：mmap 整个文件，跳过 32B 文件头与 kMeta，逐条 `DecodeEvent`
 *     （逐条 CRC 校验），遇到 kEnd 停止；
 *   - 段模式：读 `<base>.manifest` 得到每段的 seq/时间范围，范围查询先按
 *     段级索引二分/线性筛选，再 mmap 选中的段顺序解码。
 *
 * 校验口径：
 *   - 每条记录走 `replay::DecodeEvent`，CRC 错误即 `crc_ok=false`；
 *   - `seq` 必须严格递增（跨段连续），否则 `seq_ok=false`；
 *   - `checksum` 只累加资源事件，与写入端 `MixValue` 一致；
 *   - manifest 记录的 records/bytes/checksum 作为预期值暴露给调用方比对。
 */

#include "persistence/writer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace go2cpp::persistence {

struct ReadStats {
    std::uint64_t records{0};
    std::uint64_t bytes{0};
    std::uint64_t checksum{0};
    std::uint64_t elapsed_ns{0};
    std::uint64_t segments_read{0};
    bool crc_ok{true};
    bool seq_ok{true};
    std::string error;
};

class SessionReader {
public:
    ~SessionReader();

    /// 打开会话：优先读 `<base>.manifest`，不存在时按单文件 G2CRPLY 处理。
    static std::unique_ptr<SessionReader> Open(const std::string& base,
                                               std::string* error);

    /// 全量扫描（校验 CRC / seq / checksum）。
    ReadStats ScanAll();

    /// 按 seq 闭区间扫描；段模式只解码与区间重叠的段。
    ReadStats ReadSeqRange(std::uint64_t from_seq, std::uint64_t to_seq);

    /// 按事件时间戳（ts_ns）闭区间扫描；段模式只解码与区间重叠的段。
    ReadStats ReadTimeRange(std::uint64_t from_ts, std::uint64_t to_ts);

    bool segmented() const noexcept { return m_segmented; }
    std::size_t SegmentCount() const noexcept { return m_segments.size(); }
    std::uint64_t expected_records() const noexcept { return m_expected_records; }
    std::uint64_t expected_bytes() const noexcept { return m_expected_bytes; }
    std::uint64_t expected_checksum() const noexcept {
        return m_expected_checksum;
    }
    const std::string& path() const noexcept { return m_base; }

private:
    struct SegmentFile {
        SegmentInfo info;
        std::string path;
    };

    SessionReader() = default;

    ReadStats Scan(bool by_time, std::uint64_t low, std::uint64_t high);
    static bool NeedSegment(const SegmentInfo& segment, bool by_time,
                            std::uint64_t low, std::uint64_t high);

    std::string m_base;
    bool m_segmented{false};
    bool m_single_file{false};
    std::vector<SegmentFile> m_segments;
    std::uint64_t m_expected_records{0};
    std::uint64_t m_expected_bytes{0};
    std::uint64_t m_expected_checksum{0};
};

}  // namespace go2cpp::persistence
