// reflect 分阶段微基准（定位二进制解码 / 文本编解码各阶段占比）。
//   g++ -O3 -DNDEBUG -std=c++17 -Iinclude -Itests/bench/gen \
//       tests/bench/bench_reflect_stage.cpp tests/bench/gen/tools_bench.pb.cc \
//       src/reflect/*.cpp -lprotobuf -o /tmp/bench_reflect_stage
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/reflect/reflect.hpp"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "tools_bench.pb.h"

namespace {
using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void Report(const char* name, double ns, double bytes, int iters) {
    const double mb = bytes * static_cast<double>(iters) / (1024.0 * 1024.0);
    const double secs = ns * static_cast<double>(iters) / 1e9;
    std::printf("%-38s %9.2f ns/op %8.1f MiB/s\n", name, ns, mb / secs);
    std::fflush(stdout);
}

template <typename Fn>
double TimeNs(Fn&& fn, int iters) {
    for (int i = 0; i < std::max(1, iters / 10); ++i) {
        fn();
    }
    const auto start = Clock::now();
    for (int i = 0; i < iters; ++i) {
        fn();
    }
    return SecondsSince(start) * 1e9 / static_cast<double>(iters);
}

struct LoginRequest {
    std::string user;
    std::string token;
    std::int64_t timestamp = 0;
    bool remember = false;
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
}  // namespace go2cpp::reflect

namespace {
void Bench(int iters) {
    LoginRequest login;
    login.user = "alice";
    login.token = "7f3a9c2e5b8d1f4a6c0e2b9d7f5a3c1e";
    login.timestamp = 1730600000123;
    login.remember = true;

    std::vector<std::uint8_t> binary;
    std::vector<std::uint8_t> text;
    go2cpp::reflect::ReflectCodec::Encode(login, binary);
    go2cpp::reflect::ReflectCodec::Encode(login, text, go2cpp::reflect::Encoding::kText);
    std::printf("binary=%zu text=%zu\n", binary.size(), text.size());

    {
        std::size_t field_count = 0;
        const go2cpp::reflect::FieldDescriptor* fields =
            go2cpp::reflect::FieldList<LoginRequest>::Fields(field_count);
        go2cpp::reflect::Buffer reuse;
        Report("ops_encode_loop", TimeNs([&] {
            reuse.Clear();
            for (std::size_t j = 0; j < field_count; ++j) {
                fields[j].ops.encode(&login, reuse);
            }
        }, iters), 0.0, iters);
        Report("ops_encode_compact_loop", TimeNs([&] {
            reuse.Clear();
            for (std::size_t j = 0; j < field_count; ++j) {
                fields[j].ops.encode_compact(&login, reuse);
            }
        }, iters), 0.0, iters);
        Report("serialize_buf_reuse", TimeNs([&] {
            reuse.Clear();
            (void)go2cpp::reflect::Serialize(login, reuse);
        }, iters), 0.0, iters);
        Report("serialize_compact_buf_reuse", TimeNs([&] {
            reuse.Clear();
            (void)go2cpp::reflect::SerializeCompact(login, reuse);
        }, iters), 0.0, iters);
        Report("prefix_only", TimeNs([&] {
            reuse.Clear();
            (void)reuse.PutBytes("\x04\x00\x01\x02\x03", 5);
            for (std::size_t j = 0; j < field_count; ++j) {
                fields[j].ops.encode_compact(&login, reuse);
            }
        }, iters), 0.0, iters);
    }
    Report("bin_serialize_compact",
           TimeNs([&] {
               go2cpp::reflect::Buffer buffer;
               (void)go2cpp::reflect::SerializeCompact(login, buffer);
           }, iters), 0.0, iters);
    Report("bin_serialize_described",
           TimeNs([&] {
               go2cpp::reflect::Buffer buffer;
               (void)go2cpp::reflect::Serialize(login, buffer);
           }, iters), 0.0, iters);
    Report("bin_encode_compact",
           TimeNs([&] {
               std::vector<std::uint8_t> out;
               go2cpp::reflect::ReflectCodec::Encode(login, out);
           }, iters), static_cast<double>(binary.size()), iters);
    Report("bin_encode_described",
           TimeNs([&] {
               std::vector<std::uint8_t> out;
               go2cpp::reflect::ReflectCodec::Encode(
                   login, out, go2cpp::reflect::Encoding::kBinary,
                   go2cpp::reflect::Compression::kNone,
                   go2cpp::reflect::WireFormat::kSelfDescribed);
           }, iters), static_cast<double>(binary.size()), iters);
    Report("bin_encode_compact2",
           TimeNs([&] {
               std::vector<std::uint8_t> out;
               go2cpp::reflect::ReflectCodec::Encode(login, out);
           }, iters), static_cast<double>(binary.size()), iters);

    // 二进制：完整解码
    Report("bin_decode_full",
           TimeNs([&] { LoginRequest d; go2cpp::reflect::ReflectCodec::Decode(binary, d); }, iters),
           static_cast<double>(binary.size()), iters);

    // 二进制：只解外层
    Report("bin_peek_envelope",
           TimeNs([&] {
               go2cpp::reflect::Envelope e;
               go2cpp::reflect::ReflectCodec::Peek(binary.data(), binary.size(), e);
           }, iters), static_cast<double>(binary.size()), iters);

    // 二进制：只解 payload（自描述部分）
    go2cpp::reflect::Envelope env;
    go2cpp::reflect::ReflectCodec::Peek(binary.data(), binary.size(), env);
    const std::size_t payload_bytes = env.payload.size();
    Report("bin_payload_only", TimeNs([&] {
        go2cpp::reflect::Buffer in;
        in.PutBytes(env.payload.data().data(), env.payload.size());
        LoginRequest d;
        (void)go2cpp::reflect::detail::DeserializeReflect(in, d);
    }, iters), static_cast<double>(payload_bytes), iters);

    // 二进制：只做 payload 拷贝
    Report("bin_payload_copy", TimeNs([&] {
        go2cpp::reflect::Buffer in;
        in.PutBytes(env.payload.data().data(), env.payload.size());
    }, iters), static_cast<double>(payload_bytes), iters);

    // 文本：完整编码 / 只写 payload
    Report("text_encode_full",
           TimeNs([&] {
               std::vector<std::uint8_t> out;
               go2cpp::reflect::ReflectCodec::Encode(login, out, go2cpp::reflect::Encoding::kText);
           }, iters), static_cast<double>(text.size()), iters);
    Report("text_write_payload", TimeNs([&] {
        std::string s;
        go2cpp::reflect::JsonWriter w(s);
        go2cpp::reflect::WriteJson(login, w);
    }, iters), 0.0, iters);

    // 文本：完整解码 / 只解析文档 / 解析后再写 payload
    Report("text_decode_full",
           TimeNs([&] { LoginRequest d; go2cpp::reflect::ReflectCodec::Decode(text, d); }, iters),
           static_cast<double>(text.size()), iters);

    const std::string_view text_view(reinterpret_cast<const char*>(text.data()), text.size());
    Report("text_parse_doc", TimeNs([&] {
        go2cpp::reflect::JsonValue v;
        (void)go2cpp::reflect::JsonParser::Parse(text_view, v);
    }, iters), static_cast<double>(text.size()), iters);

    go2cpp::reflect::JsonValue doc;
    (void)go2cpp::reflect::JsonParser::Parse(text_view, doc);
    const std::string payload_text = [&doc] {
        std::string out;
        go2cpp::reflect::JsonWriter w(out);
        go2cpp::reflect::WriteJsonValue(*doc.Find("payload"), w);
        return out;
    }();
    Report("text_parse_payload", TimeNs([&] {
        go2cpp::reflect::JsonValue v;
        (void)go2cpp::reflect::JsonParser::Parse(payload_text, v);
    }, iters), static_cast<double>(payload_text.size()), iters);

    // protobuf / rapidjson
    toolsbench::Login pb;
    pb.set_user(login.user);
    pb.set_token(login.token);
    pb.set_timestamp(login.timestamp);
    pb.set_remember(login.remember);
    std::string pb_bytes;
    pb.SerializeToString(&pb_bytes);
    std::printf("protobuf bytes=%zu\n", pb_bytes.size());
    Report("protobuf_serialize", TimeNs([&] {
        std::string out;
        toolsbench::Login msg;
        msg.set_user(login.user);
        msg.set_token(login.token);
        msg.set_timestamp(login.timestamp);
        msg.set_remember(login.remember);
        msg.SerializeToString(&out);
    }, iters), static_cast<double>(pb_bytes.size()), iters);
    Report("protobuf_parse", TimeNs([&] {
        toolsbench::Login msg;
        msg.ParseFromString(pb_bytes);
    }, iters), static_cast<double>(pb_bytes.size()), iters);

    const std::string doc_text = R"({"v":1,"id":101,"type":"LoginRequest","payload":{"user":"alice","token":"7f3a9c2e5b8d1f4a6c0e2b9d7f5a3c1e","timestamp":1730600000123,"remember":true}})";
    rapidjson::Document rd;
    rd.Parse(doc_text.c_str());
    Report("rapidjson_parse_doc", TimeNs([&] {
        rapidjson::Document parsed;
        parsed.Parse(doc_text.c_str());
    }, iters), static_cast<double>(doc_text.size()), iters);
    Report("rapidjson_dump_login", TimeNs([&] {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject();
        writer.Key("user"); writer.String(login.user.c_str());
        writer.Key("token"); writer.String(login.token.c_str());
        writer.Key("timestamp"); writer.Int64(login.timestamp);
        writer.Key("remember"); writer.Bool(login.remember);
        writer.EndObject();
    }, iters), 0.0, iters);
}
}  // namespace

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    Bench(200000);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
