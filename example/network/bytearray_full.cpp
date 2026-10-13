#include "go2cpp/bytearray.hpp"

#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>

// bytearray 全面版：同一个组件的 6 个使用场景。
//   1  大端定长整数         write_fuint* / read_fuint* 往返
//   2  小端定长整数         set_little_endian(true) 后的字节序
//   3  长度前缀字符串       write_string_f16 / read_string_f16
//   4  变长整数             write_varuint32 对小值与多字节值的编码
//   5  顺序读写与定位       多个字段按写入顺序读回；set_position 可回退重读
//   6  越界读取             显式抛异常，而不是返回垃圾值
//
// 说明：ByteArray 不依赖 Scheduler，可在普通线程里单独使用。

namespace {

using go2cpp::net::ByteArray;

bool scenario_big_endian() {
    std::clog << "[full][场景1] 大端定长整数往返\n";
    ByteArray buffer;
    buffer.set_little_endian(false);
    buffer.write_fuint16(0x1234U);
    buffer.write_fuint32(0xDEADBEEFU);
    buffer.write_fuint64(0x0102030405060708ULL);
    buffer.set_position(0);

    const auto a = buffer.read_fuint16();
    const auto b = buffer.read_fuint32();
    const auto c = buffer.read_fuint64();
    const bool ok = a == 0x1234U && b == 0xDEADBEEFU && c == 0x0102030405060708ULL;
    std::cout << "  16=0x" << std::hex << a << " 32=0x" << b << " 64=0x" << c
              << std::dec << "（预期原值往返）\n";
    return ok;
}

bool scenario_little_endian() {
    std::clog << "[full][场景2] 小端定长整数：字节序确实按设置生效\n";
    constexpr std::uint32_t kValue = 0x11223344U;

    ByteArray big;
    big.set_little_endian(false);
    big.write_fuint32(kValue);
    // ByteArray 没有公开的 data()；用 to_hex_string() 观察实际字节。
    const auto big_hex = big.to_hex_string();

    ByteArray little;
    little.set_little_endian(true);
    little.write_fuint32(kValue);
    const auto little_hex = little.to_hex_string();

    // 大端编码 0x11223344 首字节是 0x11，小端首字节是 0x44
    const auto lower = [](std::string text) {
        for (char& ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text;
    };
    const auto big_lower = lower(big_hex);
    const auto little_lower = lower(little_hex);
    const bool bytes_ok = big_lower.find("11223344") != std::string::npos &&
                          little_lower.find("44332211") != std::string::npos;

    little.set_position(0);
    const auto round_trip = little.read_fuint32();
    const bool ok = bytes_ok && round_trip == kValue;
    std::cout << "  大端 hex=" << big_hex << " 小端 hex=" << little_hex
              << " 小端往返=0x" << std::hex << round_trip << std::dec
              << "（预期含 11223344 / 44332211 且往返原值）\n";
    return ok;
}

bool scenario_string_f16() {
    std::clog << "[full][场景3] 长度前缀字符串\n";
    ByteArray buffer;
    buffer.set_little_endian(false);
    buffer.write_string_f16("hello");
    buffer.write_string_f16("");                 // 空串也要能往返
    buffer.write_string_f16(std::string("中文也支持"));
    buffer.set_position(0);

    const auto first = buffer.read_string_f16();
    const auto empty = buffer.read_string_f16();
    const auto chinese = buffer.read_string_f16();
    const bool ok = first == "hello" && empty.empty() && chinese == "中文也支持";
    std::cout << "  \"" << first << "\" \"" << empty << "\" \"" << chinese
              << "\"（预期 hello / 空 / 中文也支持）\n";
    return ok;
}

bool scenario_varuint() {
    std::clog << "[full][场景4] 变长整数：小值占一个字节\n";
    ByteArray small;
    small.write_varuint32(1U);
    const std::size_t small_size = small.size();

    ByteArray large;
    large.write_varuint32(300U);                 // 需要两个字节
    const std::size_t large_size = large.size();

    small.set_position(0);
    large.set_position(0);
    const auto small_value = small.read_varuint32();
    const auto large_value = large.read_varuint32();
    const bool ok = small_value == 1U && large_value == 300U &&
                    small_size == 1 && large_size == 2;
    std::cout << "  1 占 " << small_size << " 字节，300 占 " << large_size
              << " 字节（预期 1 2）\n";
    return ok;
}

bool scenario_sequential_and_seek() {
    std::clog << "[full][场景5] 顺序读写与定位回退\n";
    ByteArray buffer;
    buffer.set_little_endian(false);
    buffer.write_fuint16(7U);
    buffer.write_string_f16("tag");
    buffer.write_varuint32(1000U);

    buffer.set_position(0);
    const auto number = buffer.read_fuint16();
    const auto text = buffer.read_string_f16();
    const auto variant = buffer.read_varuint32();

    // 回到开头重读第一个字段，验证 set_position 可定位
    buffer.set_position(0);
    const auto again = buffer.read_fuint16();

    const bool ok = number == 7U && text == "tag" && variant == 1000U && again == 7U;
    std::cout << "  number=" << number << " text=" << text << " variant=" << variant
              << " 重读=" << again << "（预期 7 tag 1000 7）\n";
    return ok;
}

bool scenario_out_of_range() {
    std::clog << "[full][场景6] 越界读取：显式抛异常\n";
    bool threw_32 = false;
    bool threw_string = false;
    try {
        ByteArray empty;
        (void)empty.read_fuint32();
    } catch (const std::exception& exception) {
        threw_32 = true;
        std::cout << "  读 uint32 抛出: " << exception.what() << '\n';
    }
    try {
        ByteArray short_buffer;
        short_buffer.write_fuint16(3U);          // 声称长度 3，但只有 2 字节数据
        short_buffer.set_position(0);
        (void)short_buffer.read_string_f16();
    } catch (const std::exception&) {
        threw_string = true;
    }
    std::cout << "  空缓冲读整数抛出=" << threw_32
              << " 长度超出实际数据抛出=" << threw_string << "（预期 1 1）\n";
    return threw_32 && threw_string;
}

}  // namespace

/**
 * @brief bytearray 全面版：6 个场景，覆盖字节序、字符串、变长整数、定位与越界。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_big_endian() && ok;
    ok = scenario_little_endian() && ok;
    ok = scenario_string_f16() && ok;
    ok = scenario_varuint() && ok;
    ok = scenario_sequential_and_seek() && ok;
    ok = scenario_out_of_range() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}