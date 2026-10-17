/**
 * @file huffman.cpp
 * @brief HPACK Huffman 编解码实现（RFC 7541 §5.2）。
 *
 * 码表由 Go 的 x/net/http2/hpack/tables.go 自动提取（见文件内的 *_tables.inc），
 * 提取脚本解析 `huffmanCodes` 与 `huffmanCodeLen` 两个定长数组，避免手工转录
 * 257 项出错。
 */
#include "go2cpp/network/http2/huffman.hpp"

#include <array>
#include <cstring>

namespace go2cpp::net::http2 {

const char* ToString(HuffmanStatus status) noexcept {
    switch (status) {
        case HuffmanStatus::kOk: return "ok";
        case HuffmanStatus::kInvalidPadding: return "invalid-padding";
        case HuffmanStatus::kEosInInput: return "eos-in-input";
        case HuffmanStatus::kInvalidCode: return "invalid-code";
    }
    return "unknown";
}

namespace {

// ---------------------------------------------------------------------------
// 码表（RFC 7541 附录 B）：低位对齐，有效位宽见 kHuffmanCodeLen
// ---------------------------------------------------------------------------
#include "huffman_tables.inc"

constexpr std::size_t kEosSymbol = 256;

// ---------------------------------------------------------------------------
// 解码树：逐位下降。节点数上界 = 所有码长之和 + 1。
// ---------------------------------------------------------------------------
struct TrieNode final {
    /// 子节点下标；-1 表示不存在。
    std::int32_t child[2]{-1, -1};
    /// 叶子上的符号；-1 表示内部节点。
    std::int32_t symbol{-1};
};

/// 惰性构建的解码树（进程内单例，只读）。
const std::vector<TrieNode>& DecodeTree() {
    static const std::vector<TrieNode> tree = [] {
        std::vector<TrieNode> nodes;
        nodes.emplace_back();   // 根节点
        for (std::size_t symbol = 0; symbol <= kEosSymbol; ++symbol) {
            const std::uint32_t code = kHuffmanCode[symbol];
            const std::uint8_t bits = kHuffmanCodeLen[symbol];
            std::int32_t current = 0;
            for (int bit_index = static_cast<int>(bits) - 1; bit_index >= 0; --bit_index) {
                const int bit = static_cast<int>((code >> bit_index) & 1U);
                std::int32_t next = nodes[static_cast<std::size_t>(current)].child[bit];
                if (next < 0) {
                    nodes.emplace_back();
                    next = static_cast<std::int32_t>(nodes.size()) - 1;
                    nodes[static_cast<std::size_t>(current)].child[bit] = next;
                }
                current = next;
            }
            nodes[static_cast<std::size_t>(current)].symbol =
                static_cast<std::int32_t>(symbol);
        }
        return nodes;
    }();
    return tree;
}

}  // namespace

std::size_t HuffmanEncodedBits(const std::string& input) noexcept {
    std::size_t bits = 0;
    for (const unsigned char byte : input) {
        bits += kHuffmanCodeLen[byte];
    }
    return bits;
}

HuffmanStatus HuffmanEncode(const std::string& input, std::vector<std::uint8_t>* out) {
    if (out == nullptr) {
        return HuffmanStatus::kInvalidCode;
    }
    std::uint64_t accumulator = 0;   // 位缓冲（高位在前）
    int pending_bits = 0;
    for (const unsigned char byte : input) {
        const std::uint32_t code = kHuffmanCode[byte];
        const int bits = kHuffmanCodeLen[byte];
        accumulator = (accumulator << bits) | code;
        pending_bits += bits;
        // 每满 8 位输出一个字节
        while (pending_bits >= 8) {
            pending_bits -= 8;
            out->push_back(static_cast<std::uint8_t>((accumulator >> pending_bits) & 0xFFU));
        }
        // 保留低位，避免累加器溢出（最多留 7 位）
        if (pending_bits > 0) {
            accumulator &= ((1ULL << pending_bits) - 1ULL);
        } else {
            accumulator = 0;
        }
    }
    if (pending_bits > 0) {
        // 用 EOS 的最高位（全 1）填充剩余位
        const int shift = 8 - pending_bits;
        const std::uint8_t tail = static_cast<std::uint8_t>(
            ((accumulator << shift) | ((1U << shift) - 1U)) & 0xFFU);
        out->push_back(tail);
    }
    return HuffmanStatus::kOk;
}

HuffmanStatus HuffmanDecode(const std::uint8_t* in, std::size_t length, std::string* out) {
    if (out == nullptr || (in == nullptr && length > 0)) {
        return HuffmanStatus::kInvalidCode;
    }
    out->clear();
    const auto& tree = DecodeTree();

    std::int32_t current = 0;
    std::size_t bits_since_leaf = 0;   // 用于校验末尾填充
    bool all_ones = true;              // 当前未完成路径上的位是否全 1

    for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t byte = in[i];
        for (int bit_index = 7; bit_index >= 0; --bit_index) {
            const int bit = static_cast<int>((byte >> bit_index) & 1U);
            const std::int32_t next = tree[static_cast<std::size_t>(current)].child[bit];
            if (next < 0) {
                // 走入不存在的分支：码表与输入不一致
                return HuffmanStatus::kInvalidCode;
            }
            current = next;
            ++bits_since_leaf;
            if (bit == 0) {
                all_ones = false;
            }
            const auto symbol = tree[static_cast<std::size_t>(current)].symbol;
            if (symbol >= 0) {
                if (symbol == static_cast<std::int32_t>(kEosSymbol)) {
                    // RFC 7541 §5.2：EOS 不允许出现在输入中
                    return HuffmanStatus::kEosInInput;
                }
                out->push_back(static_cast<char>(symbol));
                current = 0;
                bits_since_leaf = 0;
                all_ones = true;
            }
        }
    }
    // 尾部校验：若停在非叶节点，剩余位必须是 EOS 的前缀（全 1）且 < 8 位
    if (current != 0) {
        if (bits_since_leaf > 7 || !all_ones) {
            return HuffmanStatus::kInvalidPadding;
        }
    }
    return HuffmanStatus::kOk;
}

}  // namespace go2cpp::net::http2
