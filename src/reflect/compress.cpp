/**
 * @file compress.cpp
 * @brief RLE 压缩实现。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/reflect/compress.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace go2cpp::reflect {
namespace {

/// @brief 从字节流读 varint；失败时不修改 `pos`。
bool ReadVarint(const std::uint8_t* data, std::size_t size, std::size_t& pos,
                std::uint64_t& value) noexcept {
    std::uint64_t result = 0;
    unsigned shift = 0;
    std::size_t cursor = pos;
    for (std::size_t index = 0; index < kMaxVarintBytes; ++index) {
        if (cursor >= size) {
            return false;
        }
        const std::uint8_t byte = data[cursor++];
        if (index == kMaxVarintBytes - 1 && byte > 1) {
            return false;
        }
        result |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            pos = cursor;
            value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

}  // namespace

bool RleCompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    if (data == nullptr && size != 0) {
        return false;
    }
    try {
        out.clear();
        Buffer prefix;
        if (!prefix.PutVarint(static_cast<std::uint64_t>(size))) {
            return false;
        }
        out = std::move(prefix.data());

        std::size_t cursor = 0;
        std::size_t literal_start = 0;
        std::size_t literal_length = 0;
        const auto flush_literals = [&]() {
            while (literal_length > 0) {
                const std::size_t chunk = std::min(literal_length, kMaxRleRun);
                out.push_back(static_cast<std::uint8_t>(chunk - 1));
                out.insert(out.end(), data + literal_start, data + literal_start + chunk);
                literal_start += chunk;
                literal_length -= chunk;
            }
        };

        while (cursor < size) {
            std::size_t run = 1;
            while (cursor + run < size && data[cursor + run] == data[cursor] && run < kMaxRleRun) {
                ++run;
            }
            if (run >= kMinRleRun) {
                flush_literals();
                out.push_back(static_cast<std::uint8_t>(0x80u | (run - 1)));
                out.push_back(data[cursor]);
                cursor += run;
                literal_start = cursor;
            } else {
                if (literal_length == 0) {
                    literal_start = cursor;
                }
                literal_length += run;
                cursor += run;
                if (literal_length >= kMaxRleRun) {
                    flush_literals();
                }
            }
        }
        flush_literals();
    } catch (const std::bad_alloc&) {
        out.clear();
        return false;
    }
    return true;
}

bool RleDecompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                   std::size_t max_output) {
    out.clear();
    if (data == nullptr || size == 0) {
        return false;
    }
    std::size_t pos = 0;
    std::uint64_t original_size = 0;
    if (!ReadVarint(data, size, pos, original_size)) {
        return false;
    }
    if (original_size > max_output || original_size > kMaxBufferBytes) {
        return false;
    }
    try {
        out.reserve(static_cast<std::size_t>(original_size));
        while (pos < size) {
            const std::uint8_t control = data[pos++];
            const std::size_t count = static_cast<std::size_t>(control & 0x7fu) + 1;
            if ((control & 0x80u) != 0) {
                if (pos >= size) {
                    out.clear();
                    return false;
                }
                const std::uint8_t value = data[pos++];
                if (out.size() + count > original_size) {
                    out.clear();
                    return false;
                }
                out.insert(out.end(), count, value);
            } else {
                if (count > size - pos || out.size() + count > original_size) {
                    out.clear();
                    return false;
                }
                out.insert(out.end(), data + pos, data + pos + count);
                pos += count;
            }
        }
        if (out.size() != original_size) {
            out.clear();
            return false;
        }
    } catch (const std::bad_alloc&) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace go2cpp::reflect
