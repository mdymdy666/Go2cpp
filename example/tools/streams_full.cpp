/**
 * @file streams_full.cpp
 * @brief `go2cpp/streams.hpp` 全面版：六个场景覆盖复制、帧、限流、拼接、旁路与多路输出。
 *
 * @details 场景：
 *   1 大文件复制（1 MiB + 逐字节校验）   4 多段流拼接（MultiReader）
 *   2 定长帧解析（含不完整尾帧）          5 边读边算（TeeReader 落盘内容必须一致）
 *   3 配置头部限流（LimitReader）         6 多路输出 + 丢弃旁路 + 规模（计时仅供参考）
 *
 * @note 计时数字只用于确认没有爆炸性劣化；性能结论必须用 Release 基准（build-bench）测量。
 * @return 全部场景通过返回 0，否则返回 1。
 */
#include "go2cpp/runtime.hpp"
#include "go2cpp/streams.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace go2cpp;
using namespace go2cpp::streams;

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

bool scenario_copy_file() {
    std::clog << "[full][场景1] 大文件复制（1 MiB）\n";
    const std::string payload = MakePayload(1024U * 1024U);
    StringReader source(payload);
    BufferWriter destination;
    const ReadResult copied = Copy(destination, source);
    const bool ok = copied.ok() && copied.count == payload.size() && destination.String() == payload;
    std::cout << "  复制字节=" << copied.count << " 内容一致=" << (destination.String() == payload)
              << "（预期 1048576 1）\n";
    return ok;
}

bool scenario_frame_parsing() {
    std::clog << "[full][场景2] 定长帧解析（含不完整尾帧）\n";
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
    // 空道的 ReadFull 应返回 EOF 而不是 ErrUnexpectedEOF
    StringReader empty("");
    const ReadResult eof = ReadFull(empty, frame, sizeof(frame));
    const bool empty_ok = eof.count == 0 && Is(eof.error, IoEof());
    const bool ok = complete == 2 && tail_reported && empty_ok;
    std::cout << "  完整帧=" << complete << " 不完整尾帧=" << tail_reported
              << " 空源为 EOF=" << empty_ok << "（预期 2 1 1）\n";
    return ok;
}

bool scenario_limited_header() {
    std::clog << "[full][场景3] 配置头部限流（LimitReader）\n";
    const std::string config = "MAGIC:0011\nbody-body-body-body";
    StringReader source(config);
    LimitedReader header(source, 11);
    char buffer[64] = {};
    const ReadResult read = header.Read(buffer, sizeof(buffer));
    const bool header_ok = read.count == 11 && std::string(buffer, 10) == "MAGIC:0011";
    const ReadResult body = source.Read(buffer, sizeof(buffer));
    const bool body_ok = body.count == config.size() - 11 &&
                         std::string(buffer, body.count) == "body-body-body-body";
    std::cout << "  头部 10 字节匹配=" << header_ok << " 剩余=" << header.Remaining()
              << " 主体字节=" << body.count << "（预期 1 0 19）\n";
    return header_ok && body_ok;
}

bool scenario_multi_reader() {
    std::clog << "[full][场景4] 多段流拼接（MultiReader）\n";
    StringReader head("HEAD");
    StringReader middle("|MID|");
    StringReader tail("TAIL");
    MultiReader joined({&head, &middle, &tail});
    const auto result = ReadAll(joined);
    const bool ok = !result.second && result.first == "HEAD|MID|TAIL";
    // 段间 EOF 必须被清掉：逐字节读也要能跨段
    StringReader first("ab");
    StringReader second("cd");
    MultiReader drip({&first, &second});
    char one[1] = {};
    std::string collected;
    while (true) {
        const ReadResult step = drip.Read(one, 1);
        if (step.count > 0) collected.push_back(one[0]);
        if (step.error) break;
    }
    const bool cross_ok = collected == "abcd";
    std::cout << "  拼接结果=\"" << result.first << "\" 逐字节跨段=" << cross_ok
              << "（预期 HEAD|MID|TAIL 1）\n";
    return ok && cross_ok;
}

bool scenario_tee_reader() {
    std::clog << "[full][场景5] 边读边算（TeeReader）\n";
    const std::string payload = MakePayload(4096);
    StringReader source(payload);
    BufferWriter mirror;
    TeeReader tee(source, mirror);
    const auto result = ReadAll(tee);
    const bool ok = !result.second && result.first == payload && mirror.String() == payload;
    std::cout << "  读入字节=" << result.first.size() << " 旁路落盘一致="
              << (mirror.String() == payload) << "（预期 4096 1）\n";
    return ok;
}

bool scenario_multi_writer_and_scale() {
    std::clog << "[full][场景6] 多路输出 + 丢弃旁路 + 规模\n";
    BufferWriter console;
    BufferWriter file;
    BufferWriter audit;
    MultiWriter multi({&console, &file, &audit});
    const std::vector<std::string> lines = {"start\n", "processing\n", "done\n"};
    bool write_ok = true;
    for (const auto& line : lines) {
        const WriteResult written = WriteString(multi, line);
        if (!written.ok() || written.count != line.size()) write_ok = false;
    }
    const bool mirror_ok = console.String() == file.String() && file.String() == audit.String() &&
                           console.String() == "start\nprocessing\ndone\n";
    const bool discard_ok = Discard().Write("noise", 5).ok();

    constexpr std::size_t kSize = 1024U * 1024U;
    const std::string payload = MakePayload(kSize);
    const auto started = Clock::now();
    StringReader source(payload);
    BufferWriter destination;
    const ReadResult copied = Copy(destination, source);
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();

    const bool ok = write_ok && mirror_ok && discard_ok && copied.ok() && copied.count == kSize &&
                    destination.String() == payload;
    std::cout << "  三路一致=" << mirror_ok << " 丢弃旁路=" << discard_ok << " 1 MiB 复制耗时="
              << elapsed_us << " µs（Debug 计时仅供参考）\n";
    return ok;
}

}  // namespace

/**
 * @brief streams 全面版：六个场景。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_copy_file() && ok;
    ok = scenario_frame_parsing() && ok;
    ok = scenario_limited_header() && ok;
    ok = scenario_multi_reader() && ok;
    ok = scenario_tee_reader() && ok;
    ok = scenario_multi_writer_and_scale() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
