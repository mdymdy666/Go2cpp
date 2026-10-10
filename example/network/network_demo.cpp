#include "go2cpp/network/framing.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include <iostream>

int main() {
    // TCP 是字节流：同一批数据可以包含多条消息，也可能把分隔符拆开。
    auto framer = go2cpp::Framer::delimiter("\r\n");
    framer.feed("first\r");
    framer.feed("\nsecond\r\n");
    for (const auto& frame : framer.take_frames()) std::cout << "frame=" << frame << '\n';
    // HTTP parser 可以逐字节喂入，完成后还能取出流水线中的剩余请求。
    go2cpp::HttpParser parser;
    go2cpp::HttpRequest request;
    const std::string wire = "GET /hello HTTP/1.1\r\nHost: example\r\nContent-Length: 0\r\n\r\n";
    for (char byte : wire) parser.feed(std::string_view(&byte, 1), request);
    std::cout << go2cpp::http::method_name(request.method) << ' ' << request.path() << '\n';
    return 0;
}
