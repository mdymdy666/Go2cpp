/**
 * @file candidate.cpp
 * @brief ICE 候选的优先级、文本编解码与排序实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/candidate.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace go2cpp::p2p {
namespace {

constexpr std::size_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::size_t kFnvPrime = 1099511628211ULL;

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> SplitWhitespace(std::string_view text) {
    std::vector<std::string> tokens;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) {
            ++index;
        }
        if (index > begin) {
            tokens.emplace_back(text.substr(begin, index - begin));
        }
    }
    return tokens;
}

bool ParseU32(const std::string& text, std::uint32_t* value) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = parsed * 10U + static_cast<std::uint64_t>(ch - '0');
        if (parsed > 0xFFFFFFFFULL) {
            return false;
        }
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

void HashBytes(std::size_t* hash, const std::uint8_t* data, std::size_t length) noexcept {
    for (std::size_t index = 0; index < length; ++index) {
        *hash ^= static_cast<std::size_t>(data[index]);
        *hash *= kFnvPrime;
    }
}

std::string Hex8(std::uint32_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(8, '0');
    for (std::size_t index = 0; index < 8; ++index) {
        text[7 - index] = kDigits[value & 0xFU];
        value >>= 4U;
    }
    return text;
}

}  // namespace

const char* CandidateTypeName(CandidateType type) noexcept {
    switch (type) {
        case CandidateType::kHost:
            return "host";
        case CandidateType::kSrflx:
            return "srflx";
        case CandidateType::kPrflx:
            return "prflx";
        case CandidateType::kRelay:
            return "relay";
    }
    return "unknown";
}

std::optional<CandidateType> ParseCandidateType(std::string_view name) {
    if (name == "host") {
        return CandidateType::kHost;
    }
    if (name == "srflx") {
        return CandidateType::kSrflx;
    }
    if (name == "prflx") {
        return CandidateType::kPrflx;
    }
    if (name == "relay") {
        return CandidateType::kRelay;
    }
    return std::nullopt;
}

std::uint32_t CandidateTypePreference(CandidateType type) noexcept {
    switch (type) {
        case CandidateType::kHost:
            return 126;
        case CandidateType::kPrflx:
            return 110;
        case CandidateType::kSrflx:
            return 100;
        case CandidateType::kRelay:
            return 0;
    }
    return 0;
}

std::uint32_t ComputeCandidatePriority(CandidateType type, std::uint32_t local_pref,
                                       std::uint8_t component) noexcept {
    const std::uint32_t clamped_local = std::min<std::uint32_t>(local_pref, 65535U);
    const std::uint32_t component_term = 256U - std::max<std::uint32_t>(component, 1U);
    return (CandidateTypePreference(type) << 24U) | (clamped_local << 8U) | component_term;
}

std::string Candidate::ToString() const {
    std::ostringstream stream;
    stream << CandidateTypeName(type) << ' ' << priority << ' ' << endpoint.ToString() << ' '
           << (base.valid() ? base.ToString() : "-") << ' '
           << (foundation.empty() ? "-" : foundation);
    return stream.str();
}

std::string Candidate::ToLine() const {
    return "cand " + ToString();
}

std::optional<Candidate> Candidate::Parse(std::string_view line, std::string* error) {
    std::vector<std::string> tokens = SplitWhitespace(line);
    if (!tokens.empty() && tokens.front() == "cand") {
        tokens.erase(tokens.begin());
    }
    if (tokens.size() < 5) {
        if (error != nullptr) {
            *error = "候选行字段不足（需要 5 个字段）: " + std::string(line);
        }
        return std::nullopt;
    }
    const auto type = ParseCandidateType(tokens[0]);
    if (!type.has_value()) {
        if (error != nullptr) {
            *error = "未知候选类型: " + tokens[0];
        }
        return std::nullopt;
    }
    std::uint32_t priority = 0;
    if (!ParseU32(tokens[1], &priority)) {
        if (error != nullptr) {
            *error = "候选优先级不是 32 位无符号整数: " + tokens[1];
        }
        return std::nullopt;
    }
    const auto endpoint = Endpoint::Parse(tokens[2]);
    if (!endpoint.has_value()) {
        if (error != nullptr) {
            *error = "候选地址非法: " + tokens[2];
        }
        return std::nullopt;
    }
    Candidate candidate;
    candidate.type = *type;
    candidate.priority = priority;
    candidate.endpoint = *endpoint;
    if (tokens[3] != "-") {
        const auto base = Endpoint::Parse(tokens[3]);
        if (!base.has_value()) {
            if (error != nullptr) {
                *error = "候选 base 地址非法: " + tokens[3];
            }
            return std::nullopt;
        }
        candidate.base = *base;
    }
    if (tokens[4] != "-") {
        candidate.foundation = tokens[4];
    }
    return candidate;
}

bool Candidate::operator==(const Candidate& other) const noexcept {
    return type == other.type && endpoint == other.endpoint && base == other.base &&
           foundation == other.foundation && priority == other.priority &&
           component == other.component;
}

void FinalizeCandidate(Candidate* candidate, std::uint32_t local_pref) {
    if (candidate == nullptr) {
        return;
    }
    if (candidate->foundation.empty()) {
        candidate->foundation = MakeFoundation(candidate->type, candidate->base.valid()
                                                                        ? candidate->base
                                                                        : candidate->endpoint);
    }
    if (candidate->priority == 0) {
        candidate->priority = ComputeCandidatePriority(candidate->type, local_pref,
                                                       candidate->component);
    }
}

std::string MakeFoundation(CandidateType type, const Endpoint& base) {
    std::size_t hash = kFnvOffsetBasis;
    const std::uint8_t type_byte = static_cast<std::uint8_t>(type);
    HashBytes(&hash, &type_byte, 1);
    const std::string text = base.valid() ? base.ToString() : "<invalid>";
    HashBytes(&hash, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return Hex8(static_cast<std::uint32_t>(hash & 0xFFFFFFFFULL));
}

void SortCandidatesByPriority(std::vector<Candidate>* candidates) {
    if (candidates == nullptr) {
        return;
    }
    std::stable_sort(candidates->begin(), candidates->end(),
                     [](const Candidate& left, const Candidate& right) {
                         return left.priority > right.priority;
                     });
}

std::string SerializeCandidates(const std::vector<Candidate>& candidates) {
    std::string text = "g2p2-candidates/1\n";
    for (const Candidate& candidate : candidates) {
        text += candidate.ToLine();
        text += '\n';
    }
    return text;
}

std::vector<Candidate> ParseCandidates(std::string_view text, std::string* error) {
    std::vector<Candidate> candidates;
    std::size_t offset = 0;
    bool first_content_line = true;
    while (offset <= text.size()) {
        const std::size_t newline = text.find('\n', offset);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::string line = Trim(text.substr(offset, end - offset));
        offset = end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (first_content_line && line.rfind("g2p2-candidates/", 0) == 0) {
            const std::string version = line.substr(std::string("g2p2-candidates/").size());
            if (version != "1") {
                if (error != nullptr) {
                    *error = "不支持的候选格式版本: " + version;
                }
                return {};
            }
            first_content_line = false;
            continue;
        }
        first_content_line = false;
        const auto candidate = Candidate::Parse(line, error);
        if (!candidate.has_value()) {
            return {};
        }
        candidates.push_back(*candidate);
    }
    if (candidates.empty() && error != nullptr && error->empty()) {
        *error = "候选列表为空";
    }
    return candidates;
}

}  // namespace go2cpp::p2p
