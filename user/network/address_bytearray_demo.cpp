#include "go2cpp/address.hpp"
#include "go2cpp/bytearray.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

/**
 * @brief 演示地址抽象与协议字节缓冲。
 * @details 这两者都不依赖调度器，可以在线程环境中单独使用。
 * @return 所有场景按预期完成时返回 0。
 */
int main() {
    // 场景一（成功路径）：IPv4 解析与端口设置。
    auto ipv4 = go2cpp::IPv4Address::Create("127.0.0.1", 8080);
    const bool ipv4_ok = ipv4 != nullptr && ipv4->port() == 8080 &&
                         ipv4->to_string().find("127.0.0.1") != std::string::npos;
    std::cout << "[address_bytearray][场景一] ipv4="
              << (ipv4 ? ipv4->to_string() : std::string{"<null>"}) << '\n';

    // 场景二（子网计算）：掩码 / 网络地址 / 广播地址。
    auto mask = ipv4 ? ipv4->subnet_mask(24) : nullptr;
    auto network = ipv4 ? ipv4->network_address(24) : nullptr;
    auto broadcast = ipv4 ? ipv4->broadcast_address(24) : nullptr;
    const bool subnet_ok = mask != nullptr && network != nullptr &&
                           broadcast != nullptr;
    std::cout << "[address_bytearray][场景二] mask="
              << (mask ? mask->to_string() : std::string{"<null>"})
              << " network="
              << (network ? network->to_string() : std::string{"<null>"})
              << " broadcast="
              << (broadcast ? broadcast->to_string() : std::string{"<null>"})
              << '\n';

    // 场景三（Unix 地址与工厂）：从 sockaddr 复制出正确的子类。
    go2cpp::UnixAddress unix_address("/tmp/go2cpp-demo.sock");
    auto copied = go2cpp::Address::Create(unix_address.addr(),
                                          unix_address.addr_len());
    const bool unix_ok = copied != nullptr && copied->family() == AF_UNIX;
    std::cout << "[address_bytearray][场景三] unix="
              << (copied ? copied->to_string() : std::string{"<null>"})
              << " family_ok=" << unix_ok << '\n';

    // 场景四（字节缓冲）：大端定长、长度前缀字符串与变长整数往返。
    go2cpp::net::ByteArray buffer;
    buffer.set_little_endian(false);
    buffer.write_fuint16(0x1234U);
    buffer.write_string_f16("hello");
    buffer.write_varuint32(300U);
    buffer.set_position(0);
    const auto number = buffer.read_fuint16();
    const auto text = buffer.read_string_f16();
    const auto variant = buffer.read_varuint32();
    const bool buffer_ok = number == 0x1234U && text == "hello" && variant == 300U;
    std::cout << "[address_bytearray][场景四] number=0x" << std::hex << number
              << std::dec << " text=" << text << " variant=" << variant << '\n';

    // 场景五（错误/边界路径）：越界读取必须显式抛出，而不是返回垃圾值。
    bool threw = false;
    try {
        go2cpp::net::ByteArray empty;
        (void)empty.read_fuint32();
    } catch (const std::exception& exception) {
        threw = true;
        std::cout << "[address_bytearray][场景五] 越界读取抛出: "
                  << exception.what() << '\n';
    }

    const bool all = ipv4_ok && subnet_ok && unix_ok && buffer_ok && threw;
    std::cout << "[address_bytearray][汇总] all=" << std::boolalpha << all << '\n';
    return all ? 0 : 1;
}
