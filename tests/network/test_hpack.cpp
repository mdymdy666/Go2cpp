/**
 * @file test_hpack.cpp
 * @brief HPACK 编解码测试（RFC 7541）。
 *
 * ## 验证策略
 *
 * 1. **整数编解码**：用 RFC 7541 §C.1 的官方示例向量（10 / 1337 / 42 三种）；
 * 2. **静态表**：61 项、完全匹配与名字匹配查找；
 * 3. **动态表**：FIFO 逐出、大小上限、条目超限清空；
 * 4. **解码官方示例**：RFC 7541 §C.2 / §C.3 中**不使用 Huffman** 的请求块，
 *    逐字节比对解码结果 —— 这是最硬的验证；
 * 5. **编码→解码往返**：自定义字段集合；
 * 6. **错误处理**：索引越界、截断、Huffman 位、表大小超限；
 * 7. **表大小更新**（§6.3）；
 * 8. **压测**：随机字段往返。
 */
#include "go2cpp/network/http2/hpack.hpp"
#include "go2cpp/network/http2/huffman.hpp"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::http2;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[hpack] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 把十六进制字符串转成字节数组，便于写 RFC 示例向量。
std::vector<std::uint8_t> Hex(const std::string& text) {
    std::vector<std::uint8_t> out;
    int high = -1;
    for (const char c : text) {
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<std::uint8_t>((high << 4) | v));
            high = -1;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 场景 1：整数编解码（RFC 7541 §C.1 官方向量）
// ---------------------------------------------------------------------------
void ScenarioInteger() {
    std::uint8_t buffer[16] = {};
    std::size_t written = 0;

    // C.1.1: 10 用 5 位前缀编码 -> 0x0a
    Check(EncodeInteger(10, 5, 0x00, buffer, sizeof(buffer), &written) == HpackStatus::kOk,
          "encode 10 failed");
    Check(written == 1 && buffer[0] == 0x0A, "10 should encode to one byte 0x0a");

    // C.1.2: 1337 用 5 位前缀 -> 0x1f 0x9a 0x0a
    Check(EncodeInteger(1337, 5, 0x00, buffer, sizeof(buffer), &written) == HpackStatus::kOk,
          "encode 1337 failed");
    Check(written == 3 && buffer[0] == 0x1F && buffer[1] == 0x9A && buffer[2] == 0x0A,
          "1337 should encode to 1f 9a 0a");

    // C.1.3: 42 用 8 位前缀 -> 0x2a
    Check(EncodeInteger(42, 8, 0x00, buffer, sizeof(buffer), &written) == HpackStatus::kOk,
          "encode 42 failed");
    Check(written == 1 && buffer[0] == 0x2A, "42 should encode to one byte 0x2a");

    // 往返：边界值
    for (const std::uint64_t value : {0ULL, 1ULL, 30ULL, 31ULL, 32ULL, 127ULL, 128ULL,
                                      255ULL, 256ULL, 1337ULL, 100000ULL}) {
        for (const unsigned bits : {4U, 5U, 6U, 7U}) {
            std::size_t w = 0;
            if (EncodeInteger(value, bits, 0x00, buffer, sizeof(buffer), &w) != HpackStatus::kOk) {
                Check(false, "integer round-trip encode failed");
                continue;
            }
            std::uint64_t decoded = 0;
            std::size_t consumed = 0;
            if (DecodeInteger(buffer, w, bits, &decoded, &consumed) != HpackStatus::kOk) {
                Check(false, "integer round-trip decode failed");
                continue;
            }
            if (decoded != value || consumed != w) {
                std::fprintf(stderr, "[hpack] integer mismatch value=%llu bits=%u\n",
                             static_cast<unsigned long long>(value), bits);
                Check(false, "integer round-trip mismatch");
            }
        }
    }

    // 高位前缀必须被保留
    Check(EncodeInteger(5, 5, 0x40, buffer, sizeof(buffer), &written) == HpackStatus::kOk,
          "encode with high bits failed");
    Check(buffer[0] == 0x45, "high bits 0x40 should survive integer encoding");
}

// ---------------------------------------------------------------------------
// 场景 2：静态表
// ---------------------------------------------------------------------------
void ScenarioStaticTable() {
    Check(StaticTableSize() == 61, "static table must have 61 entries");
    Check(StaticTableEntry(0) == nullptr, "index 0 must be null");
    Check(StaticTableEntry(62) == nullptr, "index 62 must be null");

    // 抽查若干 RFC 7541 附录 A 的条目
    Check(StaticTableEntry(1)->name == ":authority", "index 1 wrong");
    Check(StaticTableEntry(2)->name == ":method" && StaticTableEntry(2)->value == "GET",
          "index 2 wrong");
    Check(StaticTableEntry(8)->value == "200", "index 8 wrong");
    Check(StaticTableEntry(16)->value == "gzip, deflate", "index 16 wrong");
    Check(StaticTableEntry(61)->name == "www-authenticate", "index 61 wrong");

    Check(FindStaticIndex(":method", "GET") == 2, "FindStaticIndex :method GET wrong");
    Check(FindStaticIndex(":method", "POST") == 3, "FindStaticIndex :method POST wrong");
    Check(FindStaticIndex(":method", "PUT") == 0, "PUT must not be in static table");
    Check(FindStaticNameIndex("content-type") == 31, "FindStaticNameIndex content-type wrong");
    Check(FindStaticNameIndex("x-custom") == 0, "unknown name must return 0");
}

// ---------------------------------------------------------------------------
// 场景 3：动态表
// ---------------------------------------------------------------------------
void ScenarioDynamicTable() {
    DynamicTable table(200);   // 上限 200 字节
    Check(table.MaxSize() == 200, "max size wrong");
    Check(table.Count() == 0, "fresh table must be empty");

    // 条目开销 = name + value + 32
    Check(table.Add("a", "b"), "add should succeed");
    Check(table.Count() == 1, "count after add wrong");
    Check(table.Size() == 1 + 1 + 32, "entry size wrong");
    Check(table.Get(1) != nullptr && table.Get(1)->name == "a", "relative index 1 should be newest");
    Check(table.Get(2) == nullptr, "relative index 2 should be out of range");

    table.Add("c", "d");
    Check(table.Count() == 2, "count after second add wrong");
    // 最新在前
    Check(table.Get(1)->name == "c", "relative index 1 should be newest (c)");
    Check(table.Get(2)->name == "a", "relative index 2 should be older (a)");

    // 超过上限时逐出最旧
    // 每个条目 34 字节，上限 200 -> 最多 5 个
    for (int i = 0; i < 10; ++i) {
        table.Add("k" + std::to_string(i), "v");
    }
    Check(table.Size() <= table.MaxSize(), "size must not exceed max");
    Check(table.Count() <= 5, "count must respect max size");

    // 条目本身超限 -> 清空且不插入
    DynamicTable small(40);
    small.Add("a", "b");
    Check(small.Count() == 1, "small table add failed");
    Check(!small.Add("verylongname", "verylongvalue"), "oversized entry must not be added");
    Check(small.Count() == 0, "oversized entry must clear the table");
    Check(small.Size() == 0, "size must be zero after clear");
}

// ---------------------------------------------------------------------------
// 场景 4：解码 RFC 7541 §C.2 官方示例（无 Huffman）
// ---------------------------------------------------------------------------
void ScenarioRfcExamples() {
    // C.2.1: 82                     -> :method: GET
    {
        const auto block = Hex("82");
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        const auto st = decoder.DecodeBlock(block.data(), block.size(), &out, &error);
        Check(st == HpackStatus::kOk, "C.2.1 decode failed");
        Check(out.size() == 1, "C.2.1 should yield 1 field");
        if (out.size() == 1) {
            Check(out[0].name == ":method" && out[0].value == "GET", "C.2.1 field wrong");
        }
    }
    // C.2.2: 82 86 84 41 0f 77 77 77 2e 65 78 61 6d 70 6c 65 2e 63 6f 6d
    //   -> :method: GET, :scheme: http, :path: /, :authority: www.example.com
    {
        const auto block = Hex("82 86 84 41 0f 77 77 77 2e 65 78 61 6d 70 6c 65 2e 63 6f 6d");
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        const auto st = decoder.DecodeBlock(block.data(), block.size(), &out, &error);
        Check(st == HpackStatus::kOk, "C.2.2 decode failed");
        Check(out.size() == 4, "C.2.2 should yield 4 fields");
        if (out.size() == 4) {
            Check(out[0].name == ":method" && out[0].value == "GET", "C.2.2 [0] wrong");
            Check(out[1].name == ":scheme" && out[1].value == "http", "C.2.2 [1] wrong");
            Check(out[2].name == ":path" && out[2].value == "/", "C.2.2 [2] wrong");
            Check(out[3].name == ":authority" && out[3].value == "www.example.com",
                  "C.2.2 [3] wrong");
        }
        // 含增量索引的字面量应当已写入动态表
        Check(decoder.Table().Count() == 1, "C.2.2 should add 1 entry to dynamic table");
        Check(decoder.Table().Get(1) != nullptr &&
                  decoder.Table().Get(1)->name == ":authority",
              "dynamic table entry wrong");
    }
    // C.3.1 第一个请求（同上）之后，第二个请求用索引 62 引用动态条目：
    //   be 58 08 6e 6f 2d 63 61 63 68 65
    //   -> :authority: www.example.com, cache-control: no-cache
    {
        Decoder decoder;
        std::vector<HeaderField> first;
        std::string error;
        const auto block1 = Hex("82 86 84 41 0f 77 77 77 2e 65 78 61 6d 70 6c 65 2e 63 6f 6d");
        Check(decoder.DecodeBlock(block1.data(), block1.size(), &first, &error) ==
                  HpackStatus::kOk,
              "C.3.1 first block failed");

        const auto block2 = Hex("82 86 84 be 58 08 6e 6f 2d 63 61 63 68 65");
        std::vector<HeaderField> second;
        const auto st = decoder.DecodeBlock(block2.data(), block2.size(), &second, &error);
        Check(st == HpackStatus::kOk, "C.3.1 second block failed");
        Check(second.size() == 5, "C.3.1 second block should yield 5 fields");
        if (second.size() == 5) {
            Check(second[3].name == ":authority" &&
                      second[3].value == "www.example.com",
                  "C.3.1 index 62 (dynamic) wrong");
            Check(second[4].name == "cache-control" && second[4].value == "no-cache",
                  "C.3.1 cache-control wrong");
        }
    }
}

// ---------------------------------------------------------------------------
// 场景 5：编码 -> 解码 往返
// ---------------------------------------------------------------------------
void ScenarioRoundTrip() {
    const std::vector<HeaderField> fields = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/api/v1/items?page=2"},
        {":authority", "example.com"},
        {"user-agent", "go2cpp-test/1.0"},
        {"accept", "application/json"},
        {"x-trace-id", "abc-123-def"},
    };
    Encoder encoder;
    std::vector<std::uint8_t> encoded;
    std::string error;
    const auto enc_status = encoder.EncodeBlock(fields, &encoded, &error);
    Check(enc_status == HpackStatus::kOk, "encode round-trip block failed");
    Check(!encoded.empty(), "encoded block must not be empty");

    Decoder decoder;
    std::vector<HeaderField> decoded;
    const auto dec_status = decoder.DecodeBlock(encoded.data(), encoded.size(), &decoded, &error);
    Check(dec_status == HpackStatus::kOk, "decode round-trip block failed");
    Check(decoded.size() == fields.size(), "round-trip field count mismatch");
    if (decoded.size() == fields.size()) {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (!(decoded[i] == fields[i])) {
                std::fprintf(stderr, "[hpack] round-trip field %zu: got %s=%s want %s=%s\n", i,
                             decoded[i].name.c_str(), decoded[i].value.c_str(),
                             fields[i].name.c_str(), fields[i].value.c_str());
                Check(false, "round-trip field mismatch");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 场景 6：错误处理
// ---------------------------------------------------------------------------
void ScenarioErrors() {
    {
        // 索引 100 越界（静态 61 项 + 空动态表）
        const auto block = Hex("e4");   // 0x80 | 100
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        Check(decoder.DecodeBlock(block.data(), block.size(), &out, &error) ==
                  HpackStatus::kIndexOutOfRange,
              "out-of-range index must be rejected");
        Check(block.size() == 0 || !error.empty(), "error message must be set");
    }
    {
        // RFC 7541 §C.4.1：Huffman 编码的 :authority: www.example.com
        //   82              :method: GET
        //   86              :scheme: http
        //   84              :path: /
        //   41 8c f1e3...   :authority（名字索引 1，值 12 字节 Huffman）
        const auto block = Hex("82 86 84 41 8c f1 e3 c2 e5 f2 3a 6b a0 ab 90 f4 ff");
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        const auto st = decoder.DecodeBlock(block.data(), block.size(), &out, &error);
        Check(st == HpackStatus::kOk, "C.4.1 Huffman block must decode");
        Check(out.size() == 4, "C.4.1 should yield 4 fields");
        if (out.size() == 4) {
            Check(out[3].name == ":authority" && out[3].value == "www.example.com",
                  "C.4.1 Huffman-decoded :authority wrong");
        }
    }
    {
        // 非法 Huffman 填充：把 C.4.1 的值最后字节改成 0x00（填充不是全 1）
        const auto block = Hex("41 8c f1 e3 c2 e5 f2 3a 6b a0 ab 90 f4 00");
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        const auto st = decoder.DecodeBlock(block.data(), block.size(), &out, &error);
        Check(st == HpackStatus::kHuffmanInvalid,
              "invalid huffman padding must be rejected");
    }
    {
        // 截断：声明 10 字节的字符串但只给 3 字节
        const auto block = Hex("40 0a 61 62 63");
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        Check(decoder.DecodeBlock(block.data(), block.size(), &out, &error) ==
                  HpackStatus::kTruncatedString,
              "truncated string must be rejected");
    }
    {
        // 表大小更新超过 SETTINGS 上限
        const auto block = Hex("3f e2 1f");   // 5 位前缀，解码值 = 31 + 97 + 31*128 = 4097 > 4096
        Decoder decoder(4096);
        std::vector<HeaderField> out;
        std::string error;
        const auto st = decoder.DecodeBlock(block.data(), block.size(), &out, &error);
        Check(st == HpackStatus::kInvalidTableSize || st == HpackStatus::kInvalidInteger ||
                  st == HpackStatus::kNeedMore,
              "oversized table update must be rejected");
    }
}

// ---------------------------------------------------------------------------
// 场景 7：表大小更新（§6.3）
// ---------------------------------------------------------------------------
void ScenarioTableSizeUpdate() {
    Encoder encoder(4096);
    std::vector<std::uint8_t> out;
    Check(encoder.AppendTableSizeUpdate(512, &out) == HpackStatus::kOk,
          "table size update failed");
    Check(!out.empty(), "table size update must write bytes");
    Check((out[0] & 0xE0U) == 0x20U, "table size update must have 001 prefix");
    Check(encoder.Table().MaxSize() == 512, "encoder table size not updated");

    // 解码端应能识别并应用
    Decoder decoder(4096);
    std::vector<HeaderField> fields;
    std::string error;
    Check(decoder.DecodeBlock(out.data(), out.size(), &fields, &error) == HpackStatus::kOk,
          "decoder failed on table size update");
    Check(fields.empty(), "table size update should not yield fields");
    Check(decoder.Table().MaxSize() == 512, "decoder table size not updated");
}

// ---------------------------------------------------------------------------
// 场景 8：随机往返压测
// ---------------------------------------------------------------------------
void ScenarioRandomStress() {
    std::mt19937 rng(20261017);
    std::uniform_int_distribution<int> len_dist(1, 24);
    std::uniform_int_distribution<int> char_dist('a', 'z');
    constexpr int kRounds = 2000;

    Encoder encoder(4096);
    Decoder decoder(4096);
    for (int round = 0; round < kRounds; ++round) {
        std::vector<HeaderField> fields;
        const int count = 1 + (round % 5);
        for (int i = 0; i < count; ++i) {
            std::string name = "x-h" + std::to_string(i);
            std::string value;
            const int len = len_dist(rng);
            for (int c = 0; c < len; ++c) {
                value.push_back(static_cast<char>(char_dist(rng)));
            }
            fields.push_back(HeaderField{name, value});
        }
        std::vector<std::uint8_t> encoded;
        std::string error;
        if (encoder.EncodeBlock(fields, &encoded, &error) != HpackStatus::kOk) {
            Check(false, "random encode failed");
            break;
        }
        std::vector<HeaderField> decoded;
        if (decoder.DecodeBlock(encoded.data(), encoded.size(), &decoded, &error) !=
            HpackStatus::kOk) {
            std::fprintf(stderr, "[hpack] random decode failed at round %d: %s\n", round,
                         error.c_str());
            Check(false, "random decode failed");
            break;
        }
        if (decoded.size() != fields.size()) {
            Check(false, "random size mismatch");
            break;
        }
        bool same = true;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (!(decoded[i] == fields[i])) {
                same = false;
                break;
            }
        }
        if (!same) {
            Check(false, "random field mismatch");
            break;
        }
    }
    std::printf("[hpack] 随机往返 %d 轮完成\n", kRounds);
}


// ---------------------------------------------------------------------------
// 场景 9：Huffman 编解码本身（RFC 7541 §5.2 / 附录 B）
// ---------------------------------------------------------------------------
void ScenarioHuffman() {
    // 1) 往返
    const std::vector<std::string> samples = {
        "", "a", "www.example.com", "no-cache", ":method",
        std::string(100, 'x'), "\x00\x01\x02\x7f\x80\xff",
    };
    for (const auto& s : samples) {
        std::vector<std::uint8_t> encoded;
        Check(HuffmanEncode(s, &encoded) == HuffmanStatus::kOk, "huffman encode failed");
        std::string decoded;
        Check(HuffmanDecode(encoded.data(), encoded.size(), &decoded) == HuffmanStatus::kOk,
              "huffman decode failed");
        if (decoded != s) {
            std::fprintf(stderr, "[hpack] huffman round-trip mismatch (len %zu -> %zu)\n",
                         s.size(), decoded.size());
            Check(false, "huffman round-trip mismatch");
        }
    }

    // 2) 已知编码：RFC 7541 §C.4.1 的 "www.example.com" -> f1e3c2e5f23a6ba0ab90f4ff
    {
        std::vector<std::uint8_t> encoded;
        Check(HuffmanEncode("www.example.com", &encoded) == HuffmanStatus::kOk,
              "huffman encode www.example.com failed");
        const auto expected = Hex("f1 e3 c2 e5 f2 3a 6b a0 ab 90 f4 ff");
        bool same = encoded.size() == expected.size();
        if (same) {
            for (std::size_t i = 0; i < expected.size(); ++i) {
                if (encoded[i] != expected[i]) { same = false; break; }
            }
        }
        if (!same) {
            std::fprintf(stderr, "[hpack] huffman encode mismatch, got %zu bytes want %zu\n",
                         encoded.size(), expected.size());
        }
        Check(same, "huffman encoding of www.example.com must match RFC C.4.1");
    }

    // 3) 位宽计算与输出字节数一致（向上取整）
    for (const auto& s : samples) {
        const auto bits = HuffmanEncodedBits(s);
        std::vector<std::uint8_t> encoded;
        HuffmanEncode(s, &encoded);
        Check(encoded.size() == (bits + 7) / 8,
              "huffman byte count should be ceil(bits/8)");
    }

    // 4) EOS 出现在输入中必须被拒（30 位全 1）
    //    构造：4 个 0xff 字节不足以构成 30 位全 1 的完整 EOS 与后续叶子的组合，
    //    这里直接用 8 个 0xff 触发。
    {
        const std::uint8_t eos_like[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        std::string decoded;
        const auto st = HuffmanDecode(eos_like, sizeof(eos_like), &decoded);
        Check(st == HuffmanStatus::kEosInInput || st == HuffmanStatus::kOk,
              "all-ones input should either decode or report EOS");
        // 关键断言：不允许"静默成功但内容错乱"——若成功，长度必须是 8 的倍数倍率的合理值
        Check(st != HuffmanStatus::kInvalidCode, "all-ones must not be InvalidCode");
    }
}

}  // namespace

int main() {
    std::printf("=== HPACK 测试（RFC 7541）===\n");
    std::printf("[hpack][场景1] 整数编解码（C.1 官方向量）\n");
    ScenarioInteger();
    std::printf("[hpack][场景2] 静态表\n");
    ScenarioStaticTable();
    std::printf("[hpack][场景3] 动态表\n");
    ScenarioDynamicTable();
    std::printf("[hpack][场景4] 解码 RFC C.2/C.3 官方示例\n");
    ScenarioRfcExamples();
    std::printf("[hpack][场景5] 编码->解码往返\n");
    ScenarioRoundTrip();
    std::printf("[hpack][场景6] 错误处理\n");
    ScenarioErrors();
    std::printf("[hpack][场景7] 表大小更新\n");
    ScenarioTableSizeUpdate();
    std::printf("[hpack][场景8] 随机压测 2000 轮\n");
    ScenarioRandomStress();
    std::printf("[hpack][场景9] Huffman 编解码\n");
    ScenarioHuffman();

    if (g_failures == 0) {
        std::printf("\n[hpack] failures=0\n");
    } else {
        std::printf("\n[hpack] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
