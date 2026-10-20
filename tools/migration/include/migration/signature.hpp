/**
 * @file signature.hpp
 * @brief 快照签名：HMAC-SHA256 + 密钥管理（v3）。
 *
 * ## 为什么快照必须签名
 *
 * 快照里是**可恢复执行的栈与代码地址**，等价于一份可注入的代码镜像。
 * 未签名或签名错误的快照一旦被 restore 接受，攻击者只要能让目标进程读到
 * 一个文件，就能控制其执行流。因此 v3 的默认策略是：
 *
 * - `dump` 必须具备密钥，否则拒绝生成快照；
 * - `restore` / `verify` 必须验证签名，未签名或签名错误一律拒绝；
 * - 唯一例外是显式的测试开关 `MIGRATION_INSECURE_NO_SIGNATURE=1`
 *   （只用于构造"未签名"负例）。
 *
 * ## 密钥来源（按优先级）
 *
 * 1. `MIGRATION_SIGNING_KEY`：环境变量。默认按原始字节解释；以 `hex:` 开头时
 *    按十六进制解码。
 * 2. `MIGRATION_SIGNING_KEY_FILE`：文件。去掉行尾 CR/LF；若内容恰好是
 *    64 个十六进制字符则按 hex 解码，否则按原始字节解释。
 *
 * 建议 32 字节（256 bit）以上随机密钥，例如：
 *
 * ```bash
 * export MIGRATION_SIGNING_KEY="$(head -c 32 /dev/urandom | xxd -p -c 64)"
 * ```
 *
 * 环境变量会出现在 `ps`/`/proc/<pid>/environ` 中，**生产环境优先用密钥文件**
 * （权限 0400，由部署系统注入）。详见 `docs/fiber-migration.md` §12。
 *
 * ## 算法与覆盖范围
 *
 * - HMAC-SHA256（RFC 2104），密钥长度不限（>64 字节先做 SHA-256 归一化）；
 * - 认证覆盖整个快照文件，**签名段自身的 MAC 字段置 0**、CRC 字段也置 0；
 * - 文件头同时记录 `key_id = SHA-256(key)[0..16)`，恢复侧先比对 key_id，
 *   给出「密钥不匹配」还是「内容被篡改」的明确诊断；
 * - 比较用恒定时间实现，避免时序侧信道。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace migration {

/// 签名段里的算法 id。
inline constexpr std::uint32_t kSignatureAlgorithmHmacSha256 = 1;
/// MAC 长度（SHA-256 输出）。
inline constexpr std::size_t kSignatureMacSize = 32;
/// key id 长度（SHA-256(key) 的前 16 字节）。
inline constexpr std::size_t kSignatureKeyIdSize = 16;
/// 签名段固定长度：magic[8] + alg[4] + flags[4] + key_id[16] + mac[32]。
inline constexpr std::size_t kSignaturePayloadSize = 64;
/// 签名段魔数。
inline constexpr char kSignatureMagic[8] = {'G', '2', 'C', 'S', 'I',
                                            'G', '0', '1'};

/// 一组已加载的签名密钥。
struct SigningKey {
    std::vector<std::uint8_t> bytes;                          ///< 原始密钥
    std::array<std::uint8_t, kSignatureKeyIdSize> key_id{};   ///< SHA-256 前缀
    std::string source;                                       ///< env / file
    bool valid{false};
};

/**
 * @brief 按环境变量策略加载密钥。
 * @param key 输出；`valid=false` 表示未配置。
 * @param error 配置错误（存在但非法）时填写。
 * @return 配置存在且合法返回 true；完全未配置时返回 false 且 error 为空。
 */
bool LoadSigningKey(SigningKey* key, std::string* error);

/// 密钥摘要（只打印 key id，不泄露密钥内容）。
std::string DescribeSigningKey(const SigningKey& key);

/// SHA-256（FIPS 180-4）。
void Sha256(const void* data, std::size_t size, std::uint8_t out[32]);

/// HMAC-SHA256（RFC 2104）。
void HmacSha256(const std::uint8_t* key, std::size_t key_size,
                const void* data, std::size_t size, std::uint8_t out[32]);

/// 十六进制编码/解码。
std::string HexEncode(const std::uint8_t* data, std::size_t size);
bool HexDecode(const std::string& hex, std::vector<std::uint8_t>* out);

/// 用 NIST / RFC 4231 向量自检 SHA-256 与 HMAC-SHA256。
bool CryptoSelfTest(std::string* detail);

}  // namespace migration
