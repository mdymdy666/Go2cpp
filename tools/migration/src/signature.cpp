/**
 * @file signature.cpp
 * @brief 签名密钥装载与诊断。
 */
#include "migration/signature.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace migration {
namespace {

bool IsHexString(const std::string& text) {
    if (text.size() != 64) return false;
    for (char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

std::string TrimTrailingNewlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

bool ReadKeyFile(const std::string& path, std::string* content,
                 std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "无法打开密钥文件 " + path + ": " + std::strerror(errno);
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    *content = TrimTrailingNewlines(buffer.str());
    if (content->empty()) {
        *error = "密钥文件为空: " + path;
        return false;
    }
    return true;
}

/// 解析密钥文本：支持 `hex:<hex>` 前缀、纯 64 位十六进制、或原始字节。
bool ParseKeyMaterial(const std::string& text, std::vector<std::uint8_t>* bytes,
                      std::string* error) {
    if (text.rfind("hex:", 0) == 0) {
        if (!HexDecode(text.substr(4), bytes) || bytes->empty()) {
            *error = "密钥的十六进制格式非法";
            return false;
        }
        return true;
    }
    if (IsHexString(text)) {
        if (!HexDecode(text, bytes) || bytes->empty()) {
            *error = "密钥的十六进制格式非法";
            return false;
        }
        return true;
    }
    bytes->assign(text.begin(), text.end());
    return true;
}

void ComputeKeyId(const std::vector<std::uint8_t>& bytes,
                  std::array<std::uint8_t, kSignatureKeyIdSize>* key_id) {
    std::uint8_t digest[32];
    Sha256(bytes.data(), bytes.size(), digest);
    std::memcpy(key_id->data(), digest, kSignatureKeyIdSize);
}

}  // namespace

bool LoadSigningKey(SigningKey* key, std::string* error) {
    if (key == nullptr) return false;
    *key = SigningKey{};
    std::string text;
    std::string source;

    const char* from_env = std::getenv("MIGRATION_SIGNING_KEY");
    if (from_env != nullptr && from_env[0] != '\0') {
        text = from_env;
        source = "env:MIGRATION_SIGNING_KEY";
    } else {
        const char* from_file = std::getenv("MIGRATION_SIGNING_KEY_FILE");
        if (from_file != nullptr && from_file[0] != '\0') {
            if (!ReadKeyFile(from_file, &text, error)) return false;
            source = std::string("file:") + from_file;
        }
    }
    if (text.empty()) {
        // 完全没有配置：不是错误，交给调用方决定是否拒绝。
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!ParseKeyMaterial(text, &bytes, error)) return false;
    if (bytes.size() < 16) {
        *error = "签名密钥过短（至少 16 字节，建议 32 字节以上随机值），当前 " +
                 std::to_string(bytes.size()) + " 字节";
        return false;
    }
    ComputeKeyId(bytes, &key->key_id);
    key->bytes = std::move(bytes);
    key->source = std::move(source);
    key->valid = true;
    return true;
}

std::string DescribeSigningKey(const SigningKey& key) {
    if (!key.valid) return "未配置签名密钥";
    std::ostringstream os;
    os << "HMAC-SHA256 key_id="
       << HexEncode(key.key_id.data(), key.key_id.size()) << " 来源=" << key.source
       << " 密钥长度=" << key.bytes.size() << " 字节";
    return os.str();
}

}  // namespace migration
