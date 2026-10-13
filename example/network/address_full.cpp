#include "go2cpp/address.hpp"

#include <iostream>
#include <string>

// address 全面版：同一个组件的 6 个使用场景。
//   1  IPv4 解析与端口        Create + port + to_string
//   2  子网计算 /24           mask / network / broadcast
//   3  子网边界 /0 与 /32     掩码长度两端的语义
//   4  Unix 域地址            UnixAddress + family
//   5  Address::Create 工厂  从 sockaddr 复制出正确的子类
//   6  非法输入               解析失败返回空指针，而不是抛异常或返回垃圾
//
// 说明：address 不依赖 Scheduler，可在普通线程里单独使用。

namespace {

using go2cpp::Address;
using go2cpp::IPv4Address;
using go2cpp::UnixAddress;

bool scenario_ipv4_basic() {
    std::clog << "[full][场景1] IPv4 解析与端口\n";
    const auto address = IPv4Address::Create("192.168.1.10", 8443);
    const bool ok = address != nullptr && address->port() == 8443 &&
                    address->family() == AF_INET &&
                    address->to_string().find("192.168.1.10") != std::string::npos;
    std::cout << "  " << (address ? address->to_string() : std::string("<null>"))
              << " port=" << (address ? address->port() : 0)
              << "（预期含 192.168.1.10 且 port=8443）\n";
    return ok;
}

bool scenario_subnet_24() {
    std::clog << "[full][场景2] 子网计算 /24\n";
    const auto address = IPv4Address::Create("192.168.1.10", 0);
    if (!address) {
        return false;
    }
    const auto mask = address->subnet_mask(24);
    const auto network = address->network_address(24);
    const auto broadcast = address->broadcast_address(24);
    const bool ok = mask && network && broadcast &&
                    mask->to_string().find("255.255.255.0") != std::string::npos &&
                    network->to_string().find("192.168.1.0") != std::string::npos &&
                    broadcast->to_string().find("192.168.1.255") != std::string::npos;
    std::cout << "  mask=" << (mask ? mask->to_string() : std::string("<null>"))
              << " network=" << (network ? network->to_string() : std::string("<null>"))
              << " broadcast=" << (broadcast ? broadcast->to_string() : std::string("<null>"))
              << '\n';
    return ok;
}

bool scenario_subnet_boundary() {
    std::clog << "[full][场景3] 掩码长度两端：/0 与 /32\n";
    const auto address = IPv4Address::Create("10.20.30.40", 0);
    if (!address) {
        return false;
    }
    const auto mask0 = address->subnet_mask(0);
    const auto net0 = address->network_address(0);
    const auto mask32 = address->subnet_mask(32);
    const auto net32 = address->network_address(32);
    const bool ok = mask0 && net0 && mask32 && net32 &&
                    mask0->to_string().find("0.0.0.0") != std::string::npos &&
                    net0->to_string().find("0.0.0.0") != std::string::npos &&
                    mask32->to_string().find("255.255.255.255") != std::string::npos &&
                    net32->to_string().find("10.20.30.40") != std::string::npos;
    std::cout << "  /0 mask=" << (mask0 ? mask0->to_string() : std::string("<null>"))
              << " net=" << (net0 ? net0->to_string() : std::string("<null>"))
              << " /32 mask=" << (mask32 ? mask32->to_string() : std::string("<null>"))
              << " net=" << (net32 ? net32->to_string() : std::string("<null>")) << '\n';
    return ok;
}

bool scenario_unix() {
    std::clog << "[full][场景4] Unix 域地址\n";
    const UnixAddress address("/tmp/go2cpp-example.sock");
    const bool ok = address.family() == AF_UNIX &&
                    address.addr_len() > 0 &&
                    address.to_string().find("go2cpp-example.sock") != std::string::npos;
    std::cout << "  " << address.to_string()
              << " family=AF_UNIX 长度=" << address.addr_len() << '\n';
    return ok;
}

bool scenario_factory() {
    std::clog << "[full][场景5] Address::Create 工厂按 family 复制出子类\n";
    const UnixAddress source("/tmp/go2cpp-factory.sock");
    const auto copied = Address::Create(source.addr(), source.addr_len());
    const auto from_ipv4 = IPv4Address::Create("127.0.0.1", 80);
    const auto copied_v4 = from_ipv4
                               ? Address::Create(from_ipv4->addr(), from_ipv4->addr_len())
                               : nullptr;
    const bool ok = copied != nullptr && copied->family() == AF_UNIX &&
                    copied_v4 != nullptr && copied_v4->family() == AF_INET &&
                    copied->to_string() == source.to_string();
    std::cout << "  unix 复制 family="
              << (copied ? static_cast<int>(copied->family()) : -1)
              << " ipv4 复制 family="
              << (copied_v4 ? static_cast<int>(copied_v4->family()) : -1)
              << "（预期 AF_UNIX / AF_INET）\n";
    return ok;
}

bool scenario_invalid_input() {
    std::clog << "[full][场景6] 非法输入：解析失败返回空指针\n";
    const auto bad = IPv4Address::Create("not-an-ip", 80);
    // 越界的掩码长度也应当被安全处理（返回空或规范化，不应崩溃）
    const auto address = IPv4Address::Create("192.168.1.1", 0);
    const auto odd_mask = address ? address->subnet_mask(64) : nullptr;
    const bool ok = bad == nullptr;
    std::cout << "  解析 \"not-an-ip\" 得到空指针=" << (bad == nullptr)
              << " 掩码 64 的结果="
              << (odd_mask ? odd_mask->to_string() : std::string("<null>"))
              << "（预期 1）\n";
    return ok;
}

}  // namespace

/**
 * @brief address 全面版：6 个场景，覆盖 IPv4/Unix、子网计算、工厂与错误输入。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_ipv4_basic() && ok;
    ok = scenario_subnet_24() && ok;
    ok = scenario_subnet_boundary() && ok;
    ok = scenario_unix() && ok;
    ok = scenario_factory() && ok;
    ok = scenario_invalid_input() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
