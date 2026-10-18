/**
 * @file bufio.cpp
 * @brief `bufio` 模块的非模板实现：哨兵错误 + 三个预置 `SplitFunc`。
 *
 * @details **为什么这些不能放头文件**：
 *   1. 哨兵错误靠指针身份比较（`Is(err, ErrTooLong())`）。头文件内联会让
 *      每个翻译单元拿到不同对象，跨单元比较静默失效；必须全进程一份。
 *   2. `SplitFunc` 是 `std::function` 的目标，闭包/函数指针需要唯一定义，
 *      放在 `.cpp` 可以避免头文件里出现重复符号或每个单元生成一份代码。
 *
 *   错误文本直接沿用 Go 标准库的措辞，便于对照与 grep：
 *   - `bufio.ErrBufferFull` = "bufio: buffer full"
 *   - `bufio.ErrNegativeCount` = "bufio: negative count"
 *   - `bufio.ErrTooLong` = "bufio.Scanner: token too long"
 */
#include "go2cpp/bufio/errors.hpp"
#include "go2cpp/bufio/scanner.hpp"

#include <cstring>

namespace go2cpp::bufio {

namespace {

/// Go `ScanLines` 里的 `dropCR`：去掉行尾 `\r`，用于 CRLF 输入。
ByteSpan DropCarriageReturn(ByteSpan token) {
    if (!token.empty() && token[token.size() - 1] == '\r') {
        return token.first(token.size() - 1);
    }
    return token;
}

/// ASCII 空白集合，对齐 Go `unicode.IsSpace` 在 ASCII 范围内的行为。
bool IsAsciiSpace(std::uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' ||
           byte == '\v' || byte == '\f';
}

}  // namespace

// 每个哨兵都用**自己的**函数内静态指针：故意泄漏、进程内唯一。
// 注意不能抽成 helper——helper 里的 static 会是同一个对象，四个错误就全串了。
ErrorPtr ErrBufferFull() {
    static const auto* const kOwned = new ErrorPtr(NewError("bufio: buffer full"));
    return *kOwned;
}

ErrorPtr ErrNegativeCount() {
    static const auto* const kOwned = new ErrorPtr(NewError("bufio: negative count"));
    return *kOwned;
}

ErrorPtr ErrTooLong() {
    static const auto* const kOwned =
        new ErrorPtr(NewError("bufio.Scanner: token too long"));
    return *kOwned;
}

ErrorPtr ErrInvalidAdvance() {
    static const auto* const kOwned = new ErrorPtr(
        NewError("bufio.Scanner: SplitFunc returns invalid advance or token"));
    return *kOwned;
}

SplitResult ScanLines(ByteSpan data, bool at_eof) {
    if (at_eof && data.empty()) {
        return SplitResult{};
    }
    if (!data.empty()) {
        const void* const found = std::memchr(data.data(), '\n', data.size());
        if (found != nullptr) {
            const std::size_t index = static_cast<std::size_t>(
                static_cast<const std::uint8_t*>(found) - data.data());
            SplitResult result;
            result.advance = index + 1;  // 消费掉 '\n'
            result.token = DropCarriageReturn(data.first(index));
            return result;
        }
    }
    if (at_eof) {
        SplitResult result;
        result.advance = data.size();
        result.token = DropCarriageReturn(data);
        return result;
    }
    // 没有换行符且未到 EOF：继续攒数据。
    return SplitResult{};
}

SplitResult ScanWords(ByteSpan data, bool at_eof) {
    std::size_t start = 0;
    while (start < data.size() && IsAsciiSpace(data[start])) {
        ++start;
    }
    for (std::size_t index = start; index < data.size(); ++index) {
        if (IsAsciiSpace(data[index])) {
            SplitResult result;
            result.advance = index + 1;  // 消费掉分隔空白
            result.token = data.subspan(start, index - start);
            return result;
        }
    }
    if (at_eof && data.size() > start) {
        SplitResult result;
        result.advance = data.size();
        result.token = data.subspan(start);
        return result;
    }
    // 只消费前导空白，为下一个词腾出空间。
    SplitResult result;
    result.advance = start;
    return result;
}

SplitResult ScanBytes(ByteSpan data, bool at_eof) {
    if (at_eof && data.empty()) {
        return SplitResult{};
    }
    SplitResult result;
    result.advance = 1;
    result.token = data.first(1);
    return result;
}

}  // namespace go2cpp::bufio
