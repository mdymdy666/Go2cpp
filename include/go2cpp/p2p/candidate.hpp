#pragma once

/**
 * @file candidate.hpp
 * @brief ICE 候选（host/srflx/prflx/relay）的表示、优先级与信令文本格式。
 * @details 优先级公式取自 RFC 8445 §5.1.2.1：
 *
 *          `priority = 2^24 × type_pref + 2^8 × local_pref + (256 - component)`
 *
 *          type_pref：host=126、prflx=110、srflx=100、relay=0。因此排序结果
 *          天然是 **host > prflx > srflx > relay**，与「先直连、后中继」的
 *          降级顺序一致。
 *
 *          候选的文本格式（信令层使用，例如 IM 消息体）：
 *
 *          ```
 *          g2p2-candidates/1
 *          cand host 2130706431 192.168.1.5:5000 192.168.1.5:5000 3f2a1c9b
 *          cand srflx 1694498815 203.0.113.7:62000 192.168.1.5:5000 91d0e4aa
 *          cand relay 16777215 198.51.100.9:49152 198.51.100.9:3478 0b7c22d1
 *          ```
 *
 *          字段依次为：类型、优先级、候选地址、base 地址、foundation。
 *          base 与 foundation 只用于诊断/分组，接收方可以不信任它们。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"

namespace go2cpp::p2p {

/// 候选类型；数值只用于线格式，不参与优先级计算。
enum class CandidateType : std::uint8_t {
    kHost = 0,   ///< 本机网卡地址。
    kSrflx = 1,  ///< 经 STUN 得到的服务器反射地址（NAT 映射）。
    kPrflx = 2,  ///< 对端通过检查包观察到的反射地址（本实现只记录，不主动收集）。
    kRelay = 3,  ///< TURN/中继地址。
};

/// 候选类型名（`host`/`srflx`/`prflx`/`relay`）。
const char* CandidateTypeName(CandidateType type) noexcept;

/// 解析候选类型名；非法返回 nullopt。
std::optional<CandidateType> ParseCandidateType(std::string_view name);

/// 类型偏好（RFC 8445：host=126、prflx=110、srflx=100、relay=0）。
std::uint32_t CandidateTypePreference(CandidateType type) noexcept;

/**
 * @brief 计算候选优先级。
 * @param local_pref 本地偏好（0..65535）；同一类型的多个候选用它区分顺序。
 * @param component 组件号，固定为 1。
 */
std::uint32_t ComputeCandidatePriority(CandidateType type, std::uint32_t local_pref = 65535,
                                       std::uint8_t component = 1) noexcept;

struct Candidate {
    CandidateType type = CandidateType::kHost;
    Endpoint endpoint;         ///< 候选地址（对端要发包的目标）。
    Endpoint base;             ///< 收集该候选时使用的本地地址（诊断/分组用）。
    std::string foundation;    ///< 同一 base + 类型的候选共享。
    std::uint32_t priority = 0;
    std::uint8_t component = 1;

    /// 单行文本（不含 `g2p2-candidates/1` 头）。
    std::string ToString() const;

    /// 带 `cand ` 前缀的单行文本。
    std::string ToLine() const;

    /// 解析单行文本（可带 `cand ` 前缀）。
    static std::optional<Candidate> Parse(std::string_view line, std::string* error = nullptr);

    bool operator==(const Candidate& other) const noexcept;
    bool operator!=(const Candidate& other) const noexcept { return !(*this == other); }
};

/// 为候选补齐 foundation 与 priority（如果调用方没有显式设置）。
void FinalizeCandidate(Candidate* candidate, std::uint32_t local_pref = 65535);

/// 生成 foundation：对 `type + base` 做 FNV-1a，输出 8 位十六进制。
std::string MakeFoundation(CandidateType type, const Endpoint& base);

/// 按优先级从高到低排序。
void SortCandidatesByPriority(std::vector<Candidate>* candidates);

/// 序列化候选列表（含版本头）。
std::string SerializeCandidates(const std::vector<Candidate>& candidates);

/**
 * @brief 解析候选列表。
 * @param text 信令消息体；允许空行与 `#` 注释行。
 * @param error 失败时写入原因（可为空）。
 * @return 解析成功的候选；格式错误时返回已解析部分并写 error？——不会：
 *         任何一行非法都整体失败并返回空列表，避免「半套候选」导致的
 *         不可预测连接行为。
 */
std::vector<Candidate> ParseCandidates(std::string_view text, std::string* error = nullptr);

}  // namespace go2cpp::p2p
