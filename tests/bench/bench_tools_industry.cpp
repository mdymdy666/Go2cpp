// Base64 / gzip / reflect(JSON) 与工业库同台基准。
// 手工编译（不进 CMake，避免影响默认构建）：
//   g++ -O3 -DNDEBUG -std=c++17 -Iinclude -Itests/bench/gen \
//       tests/bench/bench_tools_industry.cpp tests/bench/gen/tools_bench.pb.cc \
//       -L.build-bento-wsl -lgo2cpp_reflect -lgo2cpp_encoding -lgo2cpp_compress \
//       -lgo2cpp_error -lz -lprotobuf -Wl,-rpath,$PWD/.build-bento-wsl -o /tmp/bench_tools
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "compress_test_support.hpp"
#include "go2cpp/compress/compress.hpp"
#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/reflect/reflect.hpp"
#include "nlohmann/json.hpp"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "tools_bench.pb.h"

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string MakeLogText(std::size_t bytes) {
    static const char* kLines[] = {
        "2026-11-02T09:12:33.120Z INFO  scheduler fiber=1024 runq=3 latency_us=42 event=timer\n",
        "2026-11-02T09:12:33.121Z WARN  network peer=10.2.3.4:8080 retry=2 reason=timeout\n",
        "2026-11-02T09:12:33.122Z DEBUG reflect type=PlayerState bytes=61 fields=6 codec=binary\n",
        "2026-11-02T09:12:33.123Z ERROR storage leveldb status=ok compact_ms=17 files=8\n",
    };
    std::string out;
    out.reserve(bytes + 256);
    std::uint32_t state = 12345;
    while (out.size() < bytes) {
        state = state * 1664525u + 1013904223u;
        out.append(kLines[(state >> 16) % 4]);
    }
    out.resize(bytes);
    return out;
}

std::string MakeRandom(std::size_t bytes, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::string out(bytes, '\0');
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<char>(rng() & 0xffu);
    }
    return out;
}

void Report(const char* name, double ns_per_op, double bytes, int iters) {
    const double mb = bytes * static_cast<double>(iters) / (1024.0 * 1024.0);
    const double secs = ns_per_op * static_cast<double>(iters) / 1e9;
    std::printf("%-34s %10.1f ns/op  %8.1f MiB/s\n", name, ns_per_op, mb / secs);
    std::fflush(stdout);
}

template <typename Fn>
double TimeNs(Fn&& fn, int iters) {
    for (int i = 0; i < std::max(1, iters / 10); ++i) {
        fn();
    }
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < iters; ++i) {
        fn();
    }
    return SecondsSince(start) * 1e9 / static_cast<double>(iters);
}

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------
void BenchBase64(const std::string& data, int iters) {
    const go2cpp::encoding::Base64Encoding& enc = go2cpp::encoding::StdEncoding;
    std::string encoded;
    encoded.reserve(enc.EncodedLen(data.size()));
    enc.Encode(encoded, data);
    std::string decoded;
    decoded.reserve(data.size());

    const double enc_ns = TimeNs([&] {
        encoded.clear();
        enc.Encode(encoded, data);
    }, iters);
    Report("base64_encode", enc_ns, static_cast<double>(data.size()), iters);

    const double dec_ns = TimeNs([&] {
        decoded.clear();
        (void)enc.Decode(decoded, encoded);
    }, iters);
    Report("base64_decode", dec_ns, static_cast<double>(data.size()), iters);

    // 小载荷（60B token 场景）。
    const std::string small = data.substr(0, 60);
    std::string small_enc;
    const double small_ns = TimeNs([&] {
        small_enc.clear();
        enc.Encode(small_enc, small);
    }, iters * 4);
    Report("base64_encode_60B", small_ns, 60.0, iters * 4);
}

// ---------------------------------------------------------------------------
// gzip
// ---------------------------------------------------------------------------
std::string GzipCompress(const std::string& input, int level) {
    go2cpp::ErrorPtr error;
    std::string out = go2cpp_compress_test::CompressBytes(
        go2cpp_compress_test::Format::kGzip, input, level, &error);
    if (error) {
        std::printf("gzip error: %s\n", go2cpp::ErrorMessage(error).c_str());
        std::abort();
    }
    return out;
}

std::string GzipDecompress(const std::string& input) {
    go2cpp::ErrorPtr error;
    std::string out = go2cpp_compress_test::DecompressBytes(
        go2cpp_compress_test::Format::kGzip, input, &error);
    if (error) {
        std::printf("gunzip error: %s\n", go2cpp::ErrorMessage(error).c_str());
        std::abort();
    }
    return out;
}

std::string ZlibGzipCompress(const std::string& input, int level) {
    z_stream stream{};
    if (deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        std::abort();
    }
    std::string out;
    out.resize(deflateBound(&stream, static_cast<uLong>(input.size())) + 64);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&stream, Z_FINISH);
    const std::size_t produced = out.size() - stream.avail_out;
    deflateEnd(&stream);
    if (rc != Z_STREAM_END) {
        std::abort();
    }
    out.resize(produced);
    return out;
}

std::string ZlibGzipDecompress(const std::string& input) {
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        std::abort();
    }
    std::string out;
    out.resize(256 * 1024);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    for (;;) {
        const int rc = inflate(&stream, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK) {
            std::abort();
        }
        if (stream.avail_out == 0) {
            const std::size_t used = out.size();
            out.resize(used * 2);
            stream.next_out = reinterpret_cast<Bytef*>(out.data() + used);
            stream.avail_out = static_cast<uInt>(out.size() - used);
        }
    }
    out.resize(out.size() - stream.avail_out);
    inflateEnd(&stream);
    return out;
}

void BenchGzip(const std::string& text, int iters) {
    std::string packed;
    const double comp_ns = TimeNs([&] { packed = GzipCompress(text, 6); }, iters);
    Report("gzip_compress_L6", comp_ns, static_cast<double>(text.size()), iters);
    std::printf("%-34s %10zu -> %zu\n", "gzip_ratio(L6)", text.size(), packed.size());

    const double decomp_ns = TimeNs([&] {
        const std::string plain = GzipDecompress(packed);
        if (plain.size() != text.size()) {
            std::abort();
        }
    }, iters);
    Report("gzip_decompress", decomp_ns, static_cast<double>(text.size()), iters);

    std::string zpacked;
    const double zcomp_ns = TimeNs([&] { zpacked = ZlibGzipCompress(text, 6); }, iters);
    Report("zlib_gzip_compress_L6", zcomp_ns, static_cast<double>(text.size()), iters);
    std::printf("%-34s %10zu -> %zu\n", "zlib_ratio(L6)", text.size(), zpacked.size());

    const double zdecomp_ns = TimeNs([&] {
        const std::string plain = ZlibGzipDecompress(zpacked);
        if (plain.size() != text.size()) {
            std::abort();
        }
    }, iters);
    Report("zlib_gzip_decompress", zdecomp_ns, static_cast<double>(text.size()), iters);
}

// ---------------------------------------------------------------------------
// reflect JSON / binary
// ---------------------------------------------------------------------------
struct LoginRequest {
    std::string user;
    std::string token;
    std::int64_t timestamp = 0;
    bool remember = false;
};

struct ChatMessage {
    std::string room;
    std::string sender;
    std::string text;
    std::int64_t sent_at = 0;
};

}  // namespace

namespace go2cpp::reflect {

template <>
struct FieldList<LoginRequest> {
    static constexpr TypeId kId = 101;
    static constexpr const char* kName = "LoginRequest";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(LoginRequest, user),
            GO2CPP_REFLECT_FIELD(LoginRequest, token),
            GO2CPP_REFLECT_FIELD(LoginRequest, timestamp),
            GO2CPP_REFLECT_FIELD(LoginRequest, remember),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<ChatMessage> {
    static constexpr TypeId kId = 102;
    static constexpr const char* kName = "ChatMessage";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(ChatMessage, room),
            GO2CPP_REFLECT_FIELD(ChatMessage, sender),
            GO2CPP_REFLECT_FIELD(ChatMessage, text),
            GO2CPP_REFLECT_FIELD(ChatMessage, sent_at),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

}  // namespace go2cpp::reflect

namespace {

void BenchReflect(int iters) {
    LoginRequest login;
    login.user = "alice";
    login.token = "7f3a9c2e5b8d1f4a6c0e2b9d7f5a3c1e";
    login.timestamp = 1730600000123;
    login.remember = true;

    std::vector<std::uint8_t> binary;
    std::vector<std::uint8_t> text;
    go2cpp::reflect::ReflectCodec::Encode(login, binary);
    go2cpp::reflect::ReflectCodec::Encode(login, text, go2cpp::reflect::Encoding::kText);

    const double binc_ns = TimeNs([&] {
        std::vector<std::uint8_t> out;
        go2cpp::reflect::ReflectCodec::Encode(login, out);
    }, iters);
    Report("reflect_json_binary_encode", binc_ns, static_cast<double>(binary.size()), iters);

    // 同环境对照：自描述编码（旧线上格式）vs 紧凑编码（默认）。
    const double bincd_ns = TimeNs([&] {
        std::vector<std::uint8_t> out;
        go2cpp::reflect::ReflectCodec::Encode(login, out, go2cpp::reflect::Encoding::kBinary,
                                              go2cpp::reflect::Compression::kNone,
                                              go2cpp::reflect::WireFormat::kSelfDescribed);
    }, iters);
    Report("reflect_json_binary_encode_desc", bincd_ns, static_cast<double>(binary.size()), iters);

    const double bind_ns = TimeNs([&] {
        LoginRequest decoded;
        go2cpp::reflect::ReflectCodec::Decode(binary, decoded);
    }, iters);
    Report("reflect_json_binary_decode", bind_ns, static_cast<double>(binary.size()), iters);

    const double textc_ns = TimeNs([&] {
        std::vector<std::uint8_t> out;
        go2cpp::reflect::ReflectCodec::Encode(login, out, go2cpp::reflect::Encoding::kText);
    }, iters);
    Report("reflect_json_text_encode", textc_ns, static_cast<double>(text.size()), iters);

    const double textd_ns = TimeNs([&] {
        LoginRequest decoded;
        go2cpp::reflect::ReflectCodec::Decode(text, decoded);
    }, iters);
    Report("reflect_json_text_decode", textd_ns, static_cast<double>(text.size()), iters);

    // 20 条聊天消息的批（贴近真实日志/IM 场景）。
    std::vector<ChatMessage> batch;
    for (int i = 0; i < 20; ++i) {
        ChatMessage msg;
        msg.room = "room-" + std::to_string(i % 5);
        msg.sender = "user-" + std::to_string(i % 7);
        msg.text = "hello world, this is message #" + std::to_string(i);
        msg.sent_at = 1730600000000 + i;
        batch.push_back(std::move(msg));
    }
    const std::string batch_text = "[]";
    (void)batch;

    // 纯解析器：同一份 JSON 文本（nlohmann/rapidjson 同输入）。
    const std::string doc = R"({"v":1,"id":101,"type":"LoginRequest","payload":{"user":"alice","token":"7f3a9c2e5b8d1f4a6c0e2b9d7f5a3c1e","timestamp":1730600000123,"remember":true}})";
    go2cpp::reflect::JsonValue value;
    const double parse_ns = TimeNs([&] {
        go2cpp::reflect::JsonValue parsed;
        (void)go2cpp::reflect::JsonParser::Parse(doc, parsed);
    }, iters / 4);
    Report("reflect_json_parse_doc", parse_ns, static_cast<double>(doc.size()), iters / 4);

    // protobuf 等价消息。
    toolsbench::Login pb_login;
    pb_login.set_user(login.user);
    pb_login.set_token(login.token);
    pb_login.set_timestamp(login.timestamp);
    pb_login.set_remember(login.remember);
    std::string pb_bytes;
    pb_login.SerializeToString(&pb_bytes);
    const double pbs_ns = TimeNs([&] {
        std::string out;
        toolsbench::Login msg;
        msg.set_user(login.user);
        msg.set_token(login.token);
        msg.set_timestamp(login.timestamp);
        msg.set_remember(login.remember);
        msg.SerializeToString(&out);
    }, iters);
    Report("protobuf_login_serialize", pbs_ns, static_cast<double>(pb_bytes.size()), iters);
    const double pbp_ns = TimeNs([&] {
        toolsbench::Login msg;
        msg.ParseFromString(pb_bytes);
    }, iters);
    Report("protobuf_login_parse", pbp_ns, static_cast<double>(pb_bytes.size()), iters);

    // nlohmann / rapidjson 对同一登录对象的生成 + 解析。
    nlohmann::json nj;
    nj["user"] = login.user;
    nj["token"] = login.token;
    nj["timestamp"] = login.timestamp;
    nj["remember"] = login.remember;
    const std::string ntext = nj.dump();
    const double nj_dump_ns = TimeNs([&] {
        nlohmann::json out;
        out["user"] = login.user;
        out["token"] = login.token;
        out["timestamp"] = login.timestamp;
        out["remember"] = login.remember;
        (void)out.dump();
    }, iters);
    Report("nlohmann_dump_login", nj_dump_ns, static_cast<double>(ntext.size()), iters);
    const double nj_parse_ns = TimeNs([&] { (void)nlohmann::json::parse(ntext); }, iters);
    Report("nlohmann_parse_login", nj_parse_ns, static_cast<double>(ntext.size()), iters);

    const double nj_doc_parse_ns = TimeNs([&] { (void)nlohmann::json::parse(doc); }, iters / 4);
    Report("nlohmann_parse_doc", nj_doc_parse_ns, static_cast<double>(doc.size()), iters / 4);

    rapidjson::Document rd;
    rd.Parse(doc.c_str());
    const double rj_parse_ns = TimeNs([&] {
        rapidjson::Document parsed;
        parsed.Parse(doc.c_str());
    }, iters / 4);
    Report("rapidjson_parse_doc", rj_parse_ns, static_cast<double>(doc.size()), iters / 4);

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> rw(sb);
    rw.StartObject();
    rw.Key("user"); rw.String(login.user.c_str());
    rw.Key("token"); rw.String(login.token.c_str());
    rw.Key("timestamp"); rw.Int64(login.timestamp);
    rw.Key("remember"); rw.Bool(login.remember);
    rw.EndObject();
    const std::string rtext(sb.GetString(), sb.GetSize());
    const double rj_dump_ns = TimeNs([&] {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject();
        writer.Key("user"); writer.String(login.user.c_str());
        writer.Key("token"); writer.String(login.token.c_str());
        writer.Key("timestamp"); writer.Int64(login.timestamp);
        writer.Key("remember"); writer.Bool(login.remember);
        writer.EndObject();
    }, iters);
    Report("rapidjson_dump_login", rj_dump_ns, static_cast<double>(rtext.size()), iters);
}

}  // namespace

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    const std::string log_text = MakeLogText(1024 * 1024);
    const std::string random16 = MakeRandom(16 * 1024 * 1024, 42);

    BenchBase64(random16, 8);
    BenchGzip(log_text, 4);
    BenchReflect(200000);

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
