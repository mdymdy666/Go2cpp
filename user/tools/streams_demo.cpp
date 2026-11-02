/**
 * @file streams_demo.cpp
 * @brief `go2cpp/streams.hpp` 简单版：三个最常见的使用场景。
 *
 * @details 场景：
 *          ① 大文件复制（`Copy` + 逐字节校验）
 *          ② 定长帧解析（`ReadFull` 循环，含不完整尾帧）
 *          ③ 配置头部限流读取（`LimitReader`）
 * @return 全部校验通过返回 0，否则返回 1。
 */
#include "go2cpp/runtime.hpp"
#include "go2cpp/streams.hpp"

#include <cstring>
#include <iostream>
#include <string>

namespace {

using namespace go2cpp;
using namespace go2cpp::streams;

/// 造一段可重复的数据（避免依赖特定随机实现）。
std::string MakePayload(std::size_t size) {
    std::string data;
    data.reserve(size);
    unsigned int state = 0x12345678U;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525U + 1013904223U;
        data.push_back(static_cast<char>((state >> 16) & 0xFFU));
    }
    return data;
}

/// 场景 1：大文件复制。
bool scenario_copy_file() {
    std::clog << "[streams][场景1] 大文件复制（1 MiB）\n";
    const std::string payload = MakePayload(1024U * 1024U);
    StringReader source(payload);
    BufferWriter destination;
    const ReadResult copied = Copy(destination, source);
    const bool ok = copied.ok() && copied.count == payload.size() &&
                    std::memcmp(destination.String().data(), payload.data(), payload.size()) == 0;
    std::cout << "  复制字节=" << copied.count << " 内容一致="
              << (destination.String() == payload) << "（预期 1048576 1）\n";
    return ok;
}

/// 场景 2：定长帧解析。
bool scenario_frame_parsing() {
    std::clog << "[streams][场景2] 定长帧解析（ReadFull）\n";
    // 17 字节 = 2 个完整帧（各 8 字节）+ 1 字节不完整尾帧
    std::string framed = std::string(4, 'A') + std::string(4, 'B') +
                         std::string(4, 'C') + std::string(4, 'D') + "E";
    StringReader source(framed);
    char frame[8] = {};
    int complete = 0;
    bool tail_reported = false;
    while (true) {
        const ReadResult result = ReadFull(source, frame, sizeof(frame));
        if (result.error) {
            tail_reported = Is(result.error, IoErrUnexpectedEof()) && result.count == 1;
            break;
        }
        ++complete;
    }
    const bool ok = complete == 2 && tail_reported;
    std::cout << "  完整帧=" << complete << " 尾帧判定为不完整=" << tail_reported
              << "（预期 2 1）\n";
    return ok;
}

/// 场景 3：配置头部限流读取。
bool scenario_limited_header() {
    std::clog << "[streams][场景3] 配置头部限流读取（LimitReader）\n";
    const std::string config = "MAGIC:0011\nbody-body-body-body";
    StringReader source(config);
    LimitedReader header(source, 11);                 // 10 字节头部 + 换行
    char buffer[64] = {};
    const ReadResult read = header.Read(buffer, sizeof(buffer));
    const bool header_ok = read.count == 11 && std::string(buffer, 10) == "MAGIC:0011";
    const ReadResult body = source.Read(buffer, sizeof(buffer));
    const bool body_ok = body.count == config.size() - 11 &&
                         std::string(buffer, body.count) == "body-body-body-body";
    std::cout << "  头部=\"MAGIC:0011\"+换行 剩余=" << header.Remaining()
              << " 主体字节=" << body.count << "（预期 0 19）\n";
    return header_ok && body_ok;
}

}  // namespace

/**
 * @brief streams 简单版：三个场景覆盖复制、帧解析与限流。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_copy_file() && ok;
    ok = scenario_frame_parsing() && ok;
    ok = scenario_limited_header() && ok;

    std::clog << (ok ? "[streams] 全部通过\n" : "[streams] 有失败\n");
    return ok ? 0 : 1;
}
