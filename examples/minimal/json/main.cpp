// minimal_json —— 证明 `go2cpp_json` 可以**单独使用**（不拖 network/fiber/scheduler）。
//
// 验证「真的可裁剪」：
//   ldd /tmp/mj/minimal_json | grep -c 'libgo2cpp_network\|libgo2cpp_fiber\|libgo2cpp_scheduler'   # 必须是 0
#include <cstdio>
#include <string>

#include "go2cpp/json/document.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/writer.hpp"

int main() {
  // 1) 解析：标量 + 嵌套 + 中文 + \u 转义
  const std::string text =
      R"({"name":"Go2Cpp","version":1,"ok":true,"tags":["json","minimal"],"note":"\u4f60\u597d"})";
  go2cpp::json::Document doc;
  go2cpp::ErrorPtr error;
  go2cpp::json::Parser parser;
  if (!parser.Parse(text, doc, error)) {
    std::fprintf(stderr, "parse failed\n");
    return 1;
  }

  // 2) 序列化（写回）
  const std::string out = go2cpp::json::Write(doc);
  if (out.empty()) {
    std::fprintf(stderr, "write failed\n");
    return 2;
  }
  std::printf("ok: parsed %zu bytes -> wrote %zu bytes\n", text.size(), out.size());
  std::printf("out=%s\n", out.c_str());

  // 3) 只链接 go2cpp_json（+ 它自己的 go2cpp_error）——没有 network / fiber / scheduler
  return 0;
}
