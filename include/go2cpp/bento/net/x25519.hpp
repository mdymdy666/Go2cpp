#pragma once

/**
 * @file x25519.hpp
 * @brief X25519（RFC 7748）Diffie-Hellman 密钥交换：密钥生成、公钥派生与共享密钥。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 本模块只实现 **X25519 ECDH**，不实现签名、证书或任何身份认证。
 *          它解决的是「双方各自持有一对临时密钥，如何得到一个只有双方知道的
 *          32 字节共享密钥」；共享密钥如何认证、如何派生会话密钥，仍由上层
 *          握手协议负责（例如 `crypto.hpp` 的 PSK 握手）。
 *
 * ## 算法来源与许可证
 *
 * 本实现是 **TweetNaCl**（<https://tweetnacl.cr.yp.to/>，2013 版，
 * public domain）中 `crypto_scalarmult` 及其域运算辅助函数
 * （`car25519`/`sel25519`/`pack25519`/`inv25519`/`A`/`Z`/`M`/`S`）的
 * C++ 移植：16×16-bit limb 表示、carry 规则、Montgomery ladder 结构与
 * 常数时间条件交换均沿用上游。移植日期 2026-10-28；上游为 public
 * domain，本文件整体仍按 MIT 分发。
 *
 * 改动点：C++17 封装为固定 32B 接口、去掉全局状态、中间值用
 * `std::array` 承载、增加低阶点（全零共享密钥）拒绝；未改域运算的进位
 * 与条件交换逻辑。来源与改动明细另见 `AI_COLLAB.md`。
 *
 * ## 算法与安全边界
 *
 * - 曲线参数固定为 Curve25519：p = 2^255 - 19，基点 u = 9。
 * - 标量乘法使用 RFC 7748 §5 的 Montgomery ladder；私钥在每次运算时按
 *   RFC 7748 的规则 clamp：
 *     - 第 0 字节清零低 3 位（保证是 8 的倍数）；
 *     - 第 31 字节清最高位、置次高位（保证标量在 2^254 到 2^255-1 之间）。
 * - 输入 u 坐标的最高位（第 255 位）按 RFC 7748 忽略，其余 255 位作为
 *   模 p 的整数。因此同一 u 坐标的最高位被置位不会改变结果。
 * - **拒绝全零共享密钥**：低阶公钥（u = 0、1、p-1 等）会使标量乘法落到
 *   无穷远点，输出全零。`X25519ComputeSharedSecret` 把这种情况视为失败，
 *   并把输出清零；调用方必须检查返回值，不能把全零当成合法密钥。
 * - 不检查对端公钥是否在主子群上——X25519 本身没有「公钥有效性」概念；
 *   全零输出检查是 RFC 7748 建议的最低限度防护。需要更强防护时，应由上层
 *   用静态密钥/证书对临时公钥做认证（本模块不提供）。
 *
 * ## 常数时间
 *
 * Montgomery ladder 的每轮固定执行相同数量的域运算，条件交换用位掩码完成；
 * 域运算本身的分支只取决于循环下标，不取决于私钥 bit。实现不做堆分配，
 * 不调用可变时间的大数库。**但这不等于完整的侧信道安全证明**：调用方仍应
 * 避免在日志、错误信息或内存交换中泄露私钥。
 *
 * ## 与 `crypto.hpp` 的关系
 *
 * 本模块刻意不依赖 `crypto.hpp`，可单独编译/测试。未来把 X25519 接入 PSK
 * 握手时，建议在 HELLO / HELLO_ACK 中增加临时公钥字段，用
 * `X25519ComputeSharedSecret` 得到共享密钥，再与 PSK 一起送入 HChaCha20
 * 派生会话密钥；本次不改动既有 PSK 线格式，避免破坏兼容性。
 *
 * ## 使用示例
 *
 * @code
 *   std::uint8_t alice_private[32];
 *   std::uint8_t alice_public[32];
 *   X25519GeneratePrivateKey(alice_private);
 *   X25519PublicKeyFromPrivate(alice_private, alice_public);
 *
 *   // 通过不可信信道交换公钥后：
 *   std::uint8_t shared[32];
 *   if (!X25519ComputeSharedSecret(alice_private, bob_public, shared)) {
 *       // 对端公钥是低阶点，必须中止握手。
 *   }
 * @endcode
 */

#include <cstddef>
#include <cstdint>

namespace go2cpp::bento {

/// X25519 私钥、公钥与共享密钥的固定字节数。
inline constexpr std::size_t kX25519KeyBytes = 32;

/// RFC 7748 的基点 u = 9（32 字节小端编码，只有首字节为 9）。
inline constexpr std::uint8_t kX25519BasePoint[kX25519KeyBytes] = {9};

/**
 * @brief 生成 32 字节随机私钥。
 * @param private_key 输出缓冲，长度至少 `kX25519KeyBytes`；为空时不做任何事。
 *
 * @details 私钥不做 clamp，clamp 在每次标量乘法内部完成。这样私钥字节与
 *          RFC 7748 测试向量保持一致，也允许调用方把私钥原样存盘（是否允许
 *          落盘由调用方的密钥管理策略决定）。
 *
 * @note 使用 `std::random_device`；在 Linux 上它读取内核 CSPRNG。若平台实现
 *       不可用，`std::random_device` 可能抛出异常，调用方应把密钥生成放在
 *       握手失败处理路径之外。
 */
void X25519GeneratePrivateKey(std::uint8_t private_key[kX25519KeyBytes]);

/**
 * @brief 由私钥计算公钥（标量乘法，基点为 u = 9）。
 * @param private_key 32 字节私钥。
 * @param public_key 输出 32 字节公钥。
 * @return 参数非空返回 true；任一参数为空返回 false（输出不保证被修改）。
 *
 * @note 对同一私钥总是得到同一公钥；函数不修改私钥。
 */
bool X25519PublicKeyFromPrivate(const std::uint8_t private_key[kX25519KeyBytes],
                                std::uint8_t public_key[kX25519KeyBytes]) noexcept;

/**
 * @brief 计算 X25519 共享密钥。
 * @param private_key 本端 32 字节私钥。
 * @param peer_public_key 对端 32 字节公钥（最高位按 RFC 7748 忽略）。
 * @param shared_secret 输出 32 字节共享密钥。
 * @return 成功返回 true；参数为空、或结果为全零（低阶点）返回 false。
 *
 * @details 失败时 `shared_secret` 会被清零（若指针非空），调用方不能继续
 *          使用它。全零结果意味着对端公钥落在低阶子群，攻击者可能借此
 *          把共享密钥固定为已知值；必须中止握手。
 *
 * @note 函数不修改 `private_key` 与 `peer_public_key`。
 */
bool X25519ComputeSharedSecret(const std::uint8_t private_key[kX25519KeyBytes],
                               const std::uint8_t peer_public_key[kX25519KeyBytes],
                               std::uint8_t shared_secret[kX25519KeyBytes]) noexcept;

}  // namespace go2cpp::bento
