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
 *
 * ## 模块定位
 *
 * `bufio` 唯一的 `.cpp`：头文件里的 `Reader`/`Writer`/`Scanner` 都是模板，
 * 但哨兵错误的**身份**与三个预置 `SplitFunc` 的函数体必须全进程唯一，因此
 * 落在这里。任何使用 `bufio` 的程序都必须链接 `go2cpp_bufio`，否则会在
 * 链接期缺少这些符号。
 *
 * ## 本文件定义什么
 *
 * - 7 个哨兵错误工厂：`ErrBufferFull` / `ErrTooLong` / `ErrNegativeCount` /
 *   `ErrInvalidAdvance` / `ErrInvalidUnreadByte` / `ErrInvalidUnreadRune` /
 *   `ErrFinalToken`；
 * - 3 个预置 `SplitFunc`：`ScanLines`（按行）、`ScanWords`（按 ASCII 空白）、
 *   `ScanBytes`（逐字节）；
 * - 内部工具：`DropCarriageReturn`（去行尾 `\r`）与 `IsAsciiSpace`。
 *
 * ## 阅读建议
 *
 * 先看上面「为什么不能放头文件」——错误身份与 `std::function` 目标都要求
 * 唯一定义；再逐个看 `SplitFunc` 如何填 `advance`/`token`，特别是
 * `advance == 0`（继续攒数据）与 `atEOF`（交付最后一段）两条路径。
 *
 * ## 边界与陷阱
 *
 * 哨兵错误对象故意泄漏、每个函数**各自**持有静态指针；不要抽成公共 helper，
 * 否则不同错误会共享同一个 `static`。`ScanWords` 只识别 ASCII 空白，与 Go 的
 * Unicode `IsSpace` 不同（见 `scanner.hpp` 文件头）。
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

ErrorPtr ErrInvalidUnreadByte() {
    static const auto* const kOwned =
        new ErrorPtr(NewError("bufio: invalid use of UnreadByte"));
    return *kOwned;
}

ErrorPtr ErrInvalidUnreadRune() {
    static const auto* const kOwned =
        new ErrorPtr(NewError("bufio: invalid use of UnreadRune"));
    return *kOwned;
}

ErrorPtr ErrFinalToken() {
    static const auto* const kOwned =
        new ErrorPtr(NewError("bufio: final token"));
    return *kOwned;
}

/**
 * @brief 按行切分：消费到 `\n`（含），token 去掉行尾 `\r`。
 * @details 对应 Go 的 `ScanLines`；EOF 时最后一段没有 `\n` 也照常交付。
 *
 * ## 步骤
 * step1. `atEOF` 且窗口为空：没有更多 token，返回全零；
 * step2. 窗口内找 `\n`：找到就把 token 定为 `[0, index)`（去 `\r`），
 *        `advance = index + 1` 消费掉换行符；
 * step3. 没找到但已 `atEOF`：剩余全部作为最后一个 token（同样去 `\r`），
 *        `advance = data.size()`；
 * step4. 否则 `advance = 0`、无 token，告诉 Scanner「继续攒数据」。
 */
SplitResult ScanLines(ByteSpan data, bool at_eof) {
    // step1: 流末尾且没有残留数据
    if (at_eof && data.empty()) {
        return SplitResult{};
    }
    // step2: 找到换行符，交付本行
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
    // step3: EOF 时最后一段没有换行符，也要交付
    if (at_eof) {
        SplitResult result;
        result.advance = data.size();
        result.token = DropCarriageReturn(data);
        return result;
    }
    // step4: 没有换行符且未到 EOF：继续攒数据。
    return SplitResult{};
}

/**
 * @brief 按 ASCII 空白切词：跳过前导空白，token 为连续非空白字节。
 * @details `atEOF` 时最后一段没有尾随空白也照常交付；只消费前导空白时
 *          返回 `advance > 0` 且无 token，让 Scanner 继续读。
 *
 * ## 步骤
 * step1. 跳过前导 ASCII 空白，记下词首 `start`；
 * step2. 从 `start` 找下一个空白：找到则 token 为 `[start, index)`，
 *        `advance = index + 1` 消费掉分隔空白；
 * step3. 没找到但已 `atEOF` 且还有内容：整段作为最后一个词；
 * step4. 否则只消费前导空白（`advance = start`），等更多数据。
 */
SplitResult ScanWords(ByteSpan data, bool at_eof) {
    // step1: 跳过前导空白
    std::size_t start = 0;
    while (start < data.size() && IsAsciiSpace(data[start])) {
        ++start;
    }
    // step2: 找词尾空白
    for (std::size_t index = start; index < data.size(); ++index) {
        if (IsAsciiSpace(data[index])) {
            SplitResult result;
            result.advance = index + 1;  // 消费掉分隔空白
            result.token = data.subspan(start, index - start);
            return result;
        }
    }
    // step3: EOF 时最后一段没有尾随空白，也要交付
    if (at_eof && data.size() > start) {
        SplitResult result;
        result.advance = data.size();
        result.token = data.subspan(start);
        return result;
    }
    // step4: 只消费前导空白，为下一个词腾出空间。
    SplitResult result;
    result.advance = start;
    return result;
}

/**
 * @brief 逐字节切分：每次交付 1 字节。
 * @details 对应 Go 的 `ScanBytes`，主要用于把 Scanner 当「带缓冲的逐字节读」用。
 *
 * ## 步骤
 * step1. `atEOF` 且窗口为空：扫描结束；
 * step2. 否则消费 1 字节，token 就是该字节。
 */
SplitResult ScanBytes(ByteSpan data, bool at_eof) {
    // step1: 没有更多字节
    if (at_eof && data.empty()) {
        return SplitResult{};
    }
    // step2: 每次交付一个字节
    SplitResult result;
    result.advance = 1;
    result.token = data.first(1);
    return result;
}

}  // namespace go2cpp::bufio
