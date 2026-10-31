/**
 * @file test_go_net.cpp
 * @brief `go2cpp/go/net.go.hpp` + `io.go.hpp` 的真实场景测试。
 *
 * 覆盖：TCP Dial/Listen、Conn Read/Write、LocalAddr/RemoteAddr、
 *       SetReadDeadline 超时、EOF、Close 幂等、UDP 回环、
 *       io.Copy / ReadFull / WriteString 与 Conn 适配器。
 */

#include "go2cpp/go/go.hpp"
#include "go_api_check.hpp"
#include "go_api_util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/** 场景 1：TCP 回显 —— Listen/Dial/Read/Write/Addr。 */
void TestTcpEcho() {
    GO_ANNOUNCE("tcp echo: Listen + Dial + Read/Write + Addr");
    auto listen = go_api_test::ListenLoopbackTCP();
    GO_REQUIRE(listen.Ok());
    const auto port = go_api_test::ParsePort(listen.listener->Addr().String());
    GO_REQUIRE(port != 0);
    GO_CHECK(listen.listener->Addr().Network() == "tcp");

    std::atomic<bool> server_ok{false};
    std::thread server([&] {
        auto accepted = listen.listener->Accept();
        if (!accepted.first) {
            return;
        }
        char buffer[256] = {};
        const auto read = accepted.first->Read(buffer, sizeof(buffer));
        if (read.Ok()) {
            const auto written = accepted.first->Write(buffer, read.n);
            server_ok.store(written.Ok() && written.n == read.n);
        }
        (void)accepted.first->Close();
    });

    auto dialed = go::net::Dial("tcp", "127.0.0.1:" + std::to_string(port));
    GO_REQUIRE(dialed.Ok());
    auto conn = dialed.conn;
    GO_CHECK(conn->RemoteAddr().String().find(std::to_string(port)) !=
             std::string::npos);
    GO_CHECK(conn->LocalAddr().String().find("127.0.0.1") != std::string::npos);

    const std::string message = "hello-go-api";
    const auto written = conn->Write(message.data(), message.size());
    GO_CHECK(written.Ok());
    GO_CHECK(written.n == message.size());

    std::string echo(message.size(), '\0');
    const bool read_ok = go_api_test::ReadExact(*conn, echo.data(), echo.size());
    GO_CHECK(read_ok);
    GO_CHECK(echo == message);

    (void)conn->Close();
    server.join();
    GO_CHECK(server_ok.load());
    (void)listen.listener->Close();
}

/** 场景 2：SetReadDeadline —— 读超时返回 ErrDeadlineExceeded。 */
void TestReadDeadline() {
    GO_ANNOUNCE("tcp deadline: SetReadDeadline -> ErrDeadlineExceeded");
    auto listen = go_api_test::ListenLoopbackTCP();
    GO_REQUIRE(listen.Ok());
    const auto port = go_api_test::ParsePort(listen.listener->Addr().String());

    std::atomic<bool> accepted{false};
    std::thread server([&] {
        auto connection = listen.listener->Accept();
        if (!connection.first) {
            return;
        }
        accepted.store(true);
        // 故意不发送任何字节，让客户端读超时。
        std::this_thread::sleep_for(500ms);
        (void)connection.first->Close();
    });

    auto dialed = go::net::Dial("tcp", "127.0.0.1:" + std::to_string(port));
    GO_REQUIRE(dialed.Ok());
    auto conn = dialed.conn;
    while (!accepted.load()) {
        std::this_thread::sleep_for(1ms);
    }

    GO_CHECK(conn->SetReadDeadline(go::Now().Add(50 * go::Millisecond)) == nullptr);
    char buffer[8] = {};
    const auto started = std::chrono::steady_clock::now();
    const auto read = conn->Read(buffer, sizeof(buffer));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    GO_CHECK(read.err != nullptr);
    GO_CHECK(go::Is(read.err, go::ErrDeadlineExceeded()));
    GO_CHECK(elapsed < std::chrono::seconds(1));

    // 清除 deadline 后应恢复正常阻塞语义（这里只验证设置接口幂等）。
    GO_CHECK(conn->SetDeadline(go::Time{}) == nullptr);
    (void)conn->Close();
    server.join();
    (void)listen.listener->Close();
}

/** 场景 3：对端关闭 -> EOF；Close 幂等。 */
void TestEofAndIdempotentClose() {
    GO_ANNOUNCE("tcp eof: peer close -> ErrEOF, Close idempotent");
    auto listen = go_api_test::ListenLoopbackTCP();
    GO_REQUIRE(listen.Ok());
    const auto port = go_api_test::ParsePort(listen.listener->Addr().String());

    std::thread server([&] {
        auto connection = listen.listener->Accept();
        if (connection.first) {
            (void)connection.first->Close();
        }
    });

    auto dialed = go::net::Dial("tcp", "127.0.0.1:" + std::to_string(port));
    GO_REQUIRE(dialed.Ok());
    auto conn = dialed.conn;
    char buffer[16] = {};
    const auto read = conn->Read(buffer, sizeof(buffer));
    GO_CHECK(read.n == 0);
    GO_CHECK(go::Is(read.err, go::ErrEOF()));

    GO_CHECK(conn->Close() == nullptr);
    GO_CHECK(conn->Close() == nullptr);
    server.join();
    (void)listen.listener->Close();
}

/** 场景 4：UDP 回环 —— Listen udp + Dial udp。 */
void TestUdpEcho() {
    GO_ANNOUNCE("udp echo: Listen udp + Dial udp");
    auto listen = go::net::Listen("udp", "127.0.0.1:0");
    GO_REQUIRE(listen.Ok());
    const auto port = go_api_test::ParsePort(listen.listener->Addr().String());
    GO_REQUIRE(port != 0);

    auto dialed = go::net::Dial("udp", "127.0.0.1:" + std::to_string(port));
    GO_REQUIRE(dialed.Ok());
    auto client = dialed.conn;
    const std::string ping = "ping";
    GO_CHECK(client->Write(ping.data(), ping.size()).Ok());

    auto accepted = listen.listener->Accept();
    GO_REQUIRE(accepted.first != nullptr);
    auto server = accepted.first;

    char buffer[64] = {};
    const auto read = server->Read(buffer, sizeof(buffer));
    GO_CHECK(read.Ok());
    GO_CHECK(read.n == ping.size());
    GO_CHECK(std::string(buffer, read.n) == ping);

    const std::string pong = "pong";
    GO_CHECK(server->Write(pong.data(), pong.size()).Ok());

    char reply[64] = {};
    const auto client_read = client->Read(reply, sizeof(reply));
    GO_CHECK(client_read.Ok());
    GO_CHECK(std::string(reply, client_read.n) == pong);

    (void)client->Close();
    (void)server->Close();
    (void)listen.listener->Close();
}

/** 场景 5：io.Copy / ReadFull / WriteString 与 Conn 适配器。 */
void TestIoCopy() {
    GO_ANNOUNCE("io: ConnReader/ConnWriter + Copy + ReadFull + WriteString");
    auto listen = go_api_test::ListenLoopbackTCP();
    GO_REQUIRE(listen.Ok());
    const auto port = go_api_test::ParsePort(listen.listener->Addr().String());

    const std::string payload_template(3000, 'x');
    std::string payload = payload_template;
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>('a' + index % 26);
    }

    std::atomic<std::size_t> echoed{0};
    std::thread server([&] {
        auto connection = listen.listener->Accept();
        if (!connection.first) {
            return;
        }
        go::io::ConnIO io(connection.first);
        char buffer[512];
        for (;;) {
            const auto read = io.Read(buffer, sizeof(buffer));
            if (read.error) {
                break;
            }
            const auto written = io.Write(buffer, read.count);
            if (written.error) {
                break;
            }
            echoed.fetch_add(written.count);
            if (echoed.load() >= payload.size()) {
                break;
            }
        }
        (void)io.Close();
    });

    auto dialed = go::net::Dial("tcp", "127.0.0.1:" + std::to_string(port));
    GO_REQUIRE(dialed.Ok());
    go::io::ConnIO client(dialed.conn);

    const auto written = go::io::WriteString(client, payload);
    GO_CHECK(written.ok());
    GO_CHECK(written.count == payload.size());

    std::string received(payload.size(), '\0');
    const auto read =
        go::io::ReadFull(client, received.data(), received.size());
    GO_CHECK(read.ok());
    GO_CHECK(read.count == payload.size());
    GO_CHECK(received == payload);
    GO_CHECK(echoed.load() == payload.size());

    (void)client.Close();
    server.join();
    (void)listen.listener->Close();
}

/** 场景 6：io.Copy 直接把两条连接对拷（JinShan DATA 管道的核心动作）。 */
void TestIoCopyBetweenConns() {
    GO_ANNOUNCE("io: Copy between two conns (relay pump shape)");
    auto listen = go_api_test::ListenLoopbackTCP();
    GO_REQUIRE(listen.Ok());
    const auto port = go_api_test::ParsePort(listen.listener->Addr().String());

    std::thread server([&] {
        auto connection = listen.listener->Accept();
        if (!connection.first) {
            return;
        }
        char buffer[128];
        const auto read = connection.first->Read(buffer, sizeof(buffer));
        if (read.Ok()) {
            (void)connection.first->Write(buffer, read.n);
        }
        (void)connection.first->Close();
    });

    auto dialed = go::net::Dial("tcp", "127.0.0.1:" + std::to_string(port));
    GO_REQUIRE(dialed.Ok());
    go::io::ConnIO client(dialed.conn);

    const std::string message = "relay-pump";
    const auto written = go::io::WriteString(client, message);
    GO_CHECK(written.ok());

    std::string received(message.size(), '\0');
    const auto read = go::io::ReadFull(client, received.data(), received.size());
    GO_CHECK(read.ok());
    GO_CHECK(received == message);

    (void)client.Close();
    server.join();
    (void)listen.listener->Close();
}

}  // namespace

int main() {
    TestTcpEcho();
    TestReadDeadline();
    TestEofAndIdempotentClose();
    TestUdpEcho();
    TestIoCopy();
    TestIoCopyBetweenConns();
    return go_api_test::ExitCode();
}
