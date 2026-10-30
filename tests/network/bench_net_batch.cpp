/**
 * @file bench_net_batch.cpp
 * @brief 批量系统调用与零拷贝的吞吐/延迟基准。
 * @details 对比：
 *          1. UDP 回环往返：单包 API（send_to/recv_from）× 批量 API
 *             （sendmmsg/recvmmsg，批大小 1/16/64/256）的 pps 与每包摊销；
 *          2. 单包往返延迟（RTT p50/p95/p99）：单包 API vs 批量 API(count=1)；
 *          3. TCP 文件传输：sendfile 零拷贝 vs 用户态 read+send 的吞吐。
 *          每项重复 5 次取中位数，结果写入 /tmp/net_batch_bench.tsv。
 * @note 构建：g++ -std=c++20 -O2 -Iinclude tests/network/bench_net_batch.cpp \
 *             -o bench_net_batch -L<build> -lgo2cpp_network -lgo2cpp_io ...
 */

#include "go2cpp/network/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::net::IPv4Address;
using go2cpp::net::RecvBatchEntry;
using go2cpp::net::SendBatchEntry;
using go2cpp::net::Socket;

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::milliseconds;

constexpr std::size_t kPayloadSize = 64;
constexpr std::size_t kTotalPackets = 100000;
constexpr std::size_t kTcpSize = 64U << 20;  ///< 64 MiB
constexpr int kRepeats = 5;

struct UdpConfig {
    const char* name;
    std::size_t batch;
    bool single_api;
};

struct BenchRow {
    std::string name;
    std::size_t packets;
    std::size_t syscalls;
    double seconds;       ///< 中位数
    double pps;
    double ns_per_packet;
    double speedup;
};

double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::vector<std::uint8_t> MakePayloads(std::size_t count, std::size_t size) {
    std::vector<std::uint8_t> data(count * size);
    std::mt19937 generator(0x9E3779B9U);
    for (auto& byte : data) byte = static_cast<std::uint8_t>(generator() & 0xFFU);
    return data;
}

bool MakeUdpPair(Socket::Ptr* receiver, Socket::Ptr* sender,
                 go2cpp::net::AddressPtr* address) {
    *receiver = Socket::CreateUDP(AF_INET);
    if (!*receiver) return false;
    const auto bind_address = IPv4Address::Create("127.0.0.1", 0);
    if (!bind_address || !(*receiver)->bind(*bind_address)) return false;
    (*receiver)->set_recv_timeout(std::chrono::seconds(30));
    int buffer_size = 8 << 20;
    (*receiver)->set_option(SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    *address = (*receiver)->local_address();
    if (!*address) return false;
    *sender = Socket::CreateUDP(AF_INET);
    if (!*sender) return false;
    (*sender)->set_send_timeout(std::chrono::seconds(30));
    (*sender)->set_option(SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    return true;
}

/// 同步往返：每轮发 batch 个、收满 batch 个，避免接收队列堆积。
double RunUdpRoundTrip(const UdpConfig& config) {
    Socket::Ptr receiver;
    Socket::Ptr sender;
    go2cpp::net::AddressPtr address;
    if (!MakeUdpPair(&receiver, &sender, &address)) return 0.0;

    const std::size_t batch = config.batch;
    const std::size_t rounds = kTotalPackets / batch;
    const auto payloads = MakePayloads(batch, kPayloadSize);

    std::vector<SendBatchEntry> send_entries(batch);
    for (std::size_t index = 0; index < batch; ++index) {
        send_entries[index].data = payloads.data() + index * kPayloadSize;
        send_entries[index].length = kPayloadSize;
        send_entries[index].to = address->addr();
        send_entries[index].to_length = address->addr_len();
    }
    std::vector<RecvBatchEntry> recv_entries(batch);
    std::vector<std::vector<std::uint8_t>> buffers(batch,
                                                   std::vector<std::uint8_t>(kPayloadSize));
    for (std::size_t index = 0; index < batch; ++index) {
        recv_entries[index].data = buffers[index].data();
        recv_entries[index].capacity = buffers[index].size();
    }
    std::vector<std::uint8_t> single_buffer(kPayloadSize);
    go2cpp::net::AddressPtr from;

    const auto started = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
        if (config.single_api) {
            for (std::size_t index = 0; index < batch; ++index) {
                sender->send_to(payloads.data() + index * kPayloadSize, kPayloadSize,
                                *address);
            }
            std::size_t received = 0;
            while (received < batch) {
                const auto result = receiver->recv_from(single_buffer.data(),
                                                        single_buffer.size(), from, 0,
                                                        std::chrono::seconds(30));
                if (!result.ok() || result.transferred <= 0) return 0.0;
                ++received;
            }
        } else {
            std::size_t sent = 0;
            while (sent < batch) {
                const auto result = sender->send_batch(send_entries.data() + sent,
                                                       batch - sent, 0,
                                                       std::chrono::seconds(30));
                if (!result.ok() || result.transferred <= 0) return 0.0;
                sent += static_cast<std::size_t>(result.transferred);
            }
            std::size_t received = 0;
            while (received < batch) {
                const auto result = receiver->recv_batch(recv_entries.data() + received,
                                                         batch - received, 0,
                                                         std::chrono::seconds(30));
                if (!result.ok() || result.transferred <= 0) return 0.0;
                received += static_cast<std::size_t>(result.transferred);
            }
        }
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    sender->close();
    receiver->close();
    return elapsed;
}

/// 单包往返延迟（ns）：发送 1 个，收到后接收端回 1 个，发送端再收 1 个。
std::vector<double> RunUdpRtt(bool single_api, std::size_t samples) {
    Socket::Ptr server;
    Socket::Ptr client;
    go2cpp::net::AddressPtr address;
    if (!MakeUdpPair(&server, &client, &address)) return {};
    // 双方都 connect：接收端用 send() 回复，省掉每条消息解析来源地址。
    const auto client_bind = IPv4Address::Create("127.0.0.1", 0);
    if (!client_bind || !client->bind(*client_bind)) return {};
    const auto client_address = client->local_address();
    if (!client_address) return {};
    if (!client->connect(*address, std::chrono::seconds(5))) return {};
    if (!server->connect(*client_address, std::chrono::seconds(5))) return {};
    std::vector<double> latencies;
    latencies.reserve(samples);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < samples; ++index) {
        value = index;
        const auto started = Clock::now();
        go2cpp::net::SocketResult result;
        if (single_api) {
            result = client->send(&value, sizeof(value));
            if (!result.ok()) return {};
            std::uint64_t echo = 0;
            result = server->recv(&echo, sizeof(echo), 0, std::chrono::seconds(5));
            if (!result.ok()) return {};
            result = server->send(&echo, sizeof(echo));
            if (!result.ok()) return {};
            std::uint64_t reply = 0;
            result = client->recv(&reply, sizeof(reply), 0, std::chrono::seconds(5));
            if (!result.ok() || reply != value) return {};
        } else {
            SendBatchEntry entry{};
            entry.data = &value;
            entry.length = sizeof(value);
            result = client->send_batch(&entry, 1);
            if (!result.ok()) return {};
            RecvBatchEntry received{};
            std::uint64_t echo = 0;
            received.data = &echo;
            received.capacity = sizeof(echo);
            result = server->recv_batch(&received, 1, 0, std::chrono::seconds(5));
            if (!result.ok()) return {};
            SendBatchEntry reply_entry{};
            reply_entry.data = &echo;
            reply_entry.length = sizeof(echo);
            result = server->send_batch(&reply_entry, 1);
            if (!result.ok()) return {};
            RecvBatchEntry reply{};
            std::uint64_t echoed = 0;
            reply.data = &echoed;
            reply.capacity = sizeof(echoed);
            result = client->recv_batch(&reply, 1, 0, std::chrono::seconds(5));
            if (!result.ok() || echoed != value) return {};
        }
        latencies.push_back(
            std::chrono::duration<double, std::nano>(Clock::now() - started).count());
    }
    client->close();
    server->close();
    return latencies;
}

int CreateMemfd(std::size_t size) {
    const int fd = ::memfd_create("go2cpp-net-bench", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool MakeTcpPair(Socket::Ptr* listener, Socket::Ptr* client, Socket::Ptr* server) {
    *listener = Socket::CreateTCP(AF_INET);
    if (!*listener) return false;
    const auto bind_address = IPv4Address::Create("127.0.0.1", 0);
    if (!bind_address || !(*listener)->bind(*bind_address)) return false;
    if (!(*listener)->listen(4)) return false;
    const auto address = (*listener)->local_address();
    if (!address) return false;
    *client = Socket::CreateTCP(AF_INET);
    if (!*client || !(*client)->connect(*address, std::chrono::seconds(5))) return false;
    *server = (*listener)->accept(std::chrono::seconds(5));
    if (!*server) return false;
    int buffer_size = 4 << 20;
    (*client)->set_option(SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    (*server)->set_option(SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    (*client)->set_send_timeout(std::chrono::seconds(60));
    (*server)->set_recv_timeout(std::chrono::seconds(10));
    return true;
}

/// TCP 文件传输：use_sendfile=true 走 sendfile，否则用户态 read+send。
double RunTcpTransfer(bool use_sendfile) {
    Socket::Ptr listener;
    Socket::Ptr client;
    Socket::Ptr server;
    if (!MakeTcpPair(&listener, &client, &server)) return 0.0;
    const int file_fd = CreateMemfd(kTcpSize);
    if (file_fd < 0) return 0.0;
    // 每次重复都从文件头开始：sendfile(nullptr offset) 与 read 都会推进偏移，
    // 不复位会让第二次传输读到 EOF（曾导致 read+send 测得 0 MiB/s）。
    if (::lseek(file_fd, 0, SEEK_SET) < 0) {
        ::close(file_fd);
        return 0.0;
    }
    std::atomic<bool> server_ok{false};
    std::thread reader([&] {
        std::vector<std::uint8_t> buffer(256U << 10);
        std::size_t total = 0;
        while (total < kTcpSize) {
            const auto result = server->recv(buffer.data(), buffer.size(), 0,
                                             std::chrono::seconds(10));
            if (!result.ok() || result.transferred <= 0) break;
            total += static_cast<std::size_t>(result.transferred);
        }
        server_ok.store(total == kTcpSize);
    });
    const auto started = Clock::now();
    if (use_sendfile) {
        off_t offset = 0;
        client->send_file(file_fd, &offset, kTcpSize, std::chrono::seconds(60));
    } else {
        std::vector<std::uint8_t> buffer(64U << 10);
        std::size_t total = 0;
        while (total < kTcpSize) {
            const ssize_t read_count = ::read(file_fd, buffer.data(), buffer.size());
            if (read_count <= 0) break;
            // Socket::send 是一次 sendmsg，可能只写一部分；必须循环写完，
            // 否则丢弃剩余字节会让接收端永远等不满，基准测到 0。
            std::size_t written = 0;
            while (written < static_cast<std::size_t>(read_count)) {
                const auto result =
                    client->send(buffer.data() + written,
                                 static_cast<std::size_t>(read_count) - written, 0,
                                 std::chrono::seconds(60));
                if (!result.ok() || result.transferred <= 0) break;
                written += static_cast<std::size_t>(result.transferred);
            }
            total += written;
            if (written != static_cast<std::size_t>(read_count)) break;
        }
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    if (reader.joinable()) reader.join();
    ::close(file_fd);
    client->close();
    server->close();
    listener->close();
    return server_ok.load() ? elapsed : 0.0;
}

/// TCP 小消息：逐条 send（每条 1 次 syscall）vs 批量 writev（64 条 1 次 syscall）。
double RunTcpMessages(bool batched, std::size_t total_messages) {
    Socket::Ptr listener;
    Socket::Ptr client;
    Socket::Ptr server;
    if (!MakeTcpPair(&listener, &client, &server)) return 0.0;
    constexpr std::size_t kMessageSize = 64;
    constexpr std::size_t kBatch = 64;
    const std::size_t total_bytes = total_messages * kMessageSize;
    std::atomic<bool> server_ok{false};
    std::thread reader([&] {
        std::vector<std::uint8_t> buffer(256U << 10);
        std::size_t total = 0;
        while (total < total_bytes) {
            const auto result = server->recv(buffer.data(), buffer.size(), 0,
                                             std::chrono::seconds(10));
            if (!result.ok() || result.transferred <= 0) break;
            total += static_cast<std::size_t>(result.transferred);
        }
        server_ok.store(total == total_bytes);
    });
    // 消息分散在独立缓冲区：writev 不需要先 memcpy 拼成连续内存。
    std::vector<std::vector<std::uint8_t>> messages(kBatch,
                                                    std::vector<std::uint8_t>(kMessageSize, 0x5A));
    std::vector<iovec> vectors(kBatch);
    for (std::size_t index = 0; index < kBatch; ++index) {
        vectors[index].iov_base = messages[index].data();
        vectors[index].iov_len = kMessageSize;
    }
    const auto started = Clock::now();
    if (!batched) {
        for (std::size_t index = 0; index < total_messages; ++index) {
            const auto result = client->send(messages[index % kBatch].data(), kMessageSize,
                                             0, std::chrono::seconds(10));
            if (!result.ok() || result.transferred != static_cast<ssize_t>(kMessageSize)) {
                break;
            }
        }
    } else {
        const std::size_t rounds = total_messages / kBatch;
        const std::size_t expected = kBatch * kMessageSize;
        std::size_t sent_bytes = 0;
        for (std::size_t round = 0; round < rounds; ++round) {
            std::size_t batch_sent = 0;
            while (batch_sent < expected) {
                // 部分写时按字节偏移重建 iovec 视图；正常路径只调用一次 sendv。
                std::size_t skip = batch_sent;
                std::vector<iovec> view;
                view.reserve(kBatch);
                for (std::size_t index = 0; index < kBatch; ++index) {
                    if (skip >= kMessageSize) {
                        skip -= kMessageSize;
                        continue;
                    }
                    view.push_back(iovec{messages[index].data() + skip,
                                         kMessageSize - skip});
                    skip = 0;
                }
                const auto result = client->sendv(view.data(), view.size(), 0,
                                                  std::chrono::seconds(10));
                if (!result.ok() || result.transferred <= 0) {
                    batch_sent = expected;
                    break;
                }
                batch_sent += static_cast<std::size_t>(result.transferred);
            }
            sent_bytes += batch_sent;
        }
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    if (reader.joinable()) reader.join();
    client->close();
    server->close();
    listener->close();
    return server_ok.load() ? elapsed : 0.0;
}

void PrintPercentiles(const char* name, std::vector<double> latencies) {
    if (latencies.empty()) {
        std::printf("%-10s  (no samples)\n", name);
        return;
    }
    std::sort(latencies.begin(), latencies.end());
    const auto at = [&latencies](double quantile) {
        const std::size_t index = static_cast<std::size_t>(
            quantile * static_cast<double>(latencies.size() - 1));
        return latencies[index];
    };
    std::printf("%-10s p50=%7.0fns p95=%7.0fns p99=%7.0fns mean=%7.0fns\n", name,
                at(0.50), at(0.95), at(0.99),
                std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                    static_cast<double>(latencies.size()));
}

}  // namespace

int main() {
    std::printf("== UDP 回环批量收发（%zu 包，每包 %zuB，5 次中位数）==\n",
                kTotalPackets, kPayloadSize);
    const UdpConfig configs[] = {
        {"single", 1, true},
        {"batch-1", 1, false},
        {"batch-16", 16, false},
        {"batch-64", 64, false},
        {"batch-256", 256, false},
    };

    std::vector<BenchRow> rows;
    double baseline_pps = 0.0;
    for (const auto& config : configs) {
        std::vector<double> samples;
        for (int repeat = 0; repeat < kRepeats; ++repeat) {
            const double seconds = RunUdpRoundTrip(config);
            if (seconds > 0.0) samples.push_back(seconds);
        }
        const double seconds = Median(samples);
        BenchRow row;
        row.name = config.name;
        row.packets = kTotalPackets;
        row.syscalls = 2 * (kTotalPackets / config.batch);
        row.seconds = seconds;
        row.pps = seconds > 0.0 ? static_cast<double>(kTotalPackets) / seconds : 0.0;
        row.ns_per_packet = seconds > 0.0
                                ? seconds * 1e9 / static_cast<double>(kTotalPackets)
                                : 0.0;
        if (config.single_api) baseline_pps = row.pps;
        row.speedup = baseline_pps > 0.0 ? row.pps / baseline_pps : 0.0;
        rows.push_back(row);
    }

    std::printf("\n%-10s %10s %10s %12s %12s %10s\n", "mode", "packets", "syscalls",
                "pps", "ns/packet", "speedup");
    for (const auto& row : rows) {
        std::printf("%-10s %10zu %10zu %12.0f %12.1f %9.2fx\n", row.name.c_str(),
                    row.packets, row.syscalls, row.pps, row.ns_per_packet, row.speedup);
    }

    std::printf("\n== 单包往返延迟（1000 样本）==\n");
    PrintPercentiles("single", RunUdpRtt(true, 1000));
    PrintPercentiles("batch-1", RunUdpRtt(false, 1000));

    std::printf("\n== TCP 文件传输（%zu MiB，5 次中位数）==\n", kTcpSize >> 20);
    double sendfile_seconds = 0.0;
    double copy_seconds = 0.0;
    {
        std::vector<double> samples;
        for (int repeat = 0; repeat < kRepeats; ++repeat) {
            const double seconds = RunTcpTransfer(true);
            if (seconds > 0.0) samples.push_back(seconds);
        }
        sendfile_seconds = Median(samples);
    }
    {
        std::vector<double> samples;
        for (int repeat = 0; repeat < kRepeats; ++repeat) {
            const double seconds = RunTcpTransfer(false);
            if (seconds > 0.0) samples.push_back(seconds);
        }
        copy_seconds = Median(samples);
    }
    const double mib = static_cast<double>(kTcpSize) / (1024.0 * 1024.0);
    const double sendfile_mibs = sendfile_seconds > 0.0 ? mib / sendfile_seconds : 0.0;
    const double copy_mibs = copy_seconds > 0.0 ? mib / copy_seconds : 0.0;
    std::printf("sendfile   %.3fs  %.1f MiB/s\n", sendfile_seconds, sendfile_mibs);
    std::printf("read+send  %.3fs  %.1f MiB/s\n", copy_seconds, copy_mibs);
    std::printf("speedup    %.2fx\n", copy_mibs > 0.0 ? sendfile_mibs / copy_mibs : 0.0);

    std::printf("\n== TCP 小消息：逐条 send vs 批量 writev（200000 条 64B，5 次中位数）==\n");
    constexpr std::size_t kMessages = 200000;
    double message_single_seconds = 0.0;
    double message_batch_seconds = 0.0;
    {
        std::vector<double> samples;
        for (int repeat = 0; repeat < kRepeats; ++repeat) {
            const double seconds = RunTcpMessages(false, kMessages);
            if (seconds > 0.0) samples.push_back(seconds);
        }
        message_single_seconds = Median(samples);
    }
    {
        std::vector<double> samples;
        for (int repeat = 0; repeat < kRepeats; ++repeat) {
            const double seconds = RunTcpMessages(true, kMessages);
            if (seconds > 0.0) samples.push_back(seconds);
        }
        message_batch_seconds = Median(samples);
    }
    const double message_single_pps =
        message_single_seconds > 0.0 ? static_cast<double>(kMessages) / message_single_seconds : 0.0;
    const double message_batch_pps =
        message_batch_seconds > 0.0 ? static_cast<double>(kMessages) / message_batch_seconds : 0.0;
    std::printf("send       %.3fs  %.0f msg/s  %.0f ns/msg\n", message_single_seconds,
                message_single_pps,
                message_single_seconds > 0.0 ? message_single_seconds * 1e9 / kMessages : 0.0);
    std::printf("writev-64  %.3fs  %.0f msg/s  %.0f ns/msg\n", message_batch_seconds,
                message_batch_pps,
                message_batch_seconds > 0.0 ? message_batch_seconds * 1e9 / kMessages : 0.0);
    std::printf("speedup    %.2fx\n",
                message_single_pps > 0.0 ? message_batch_pps / message_single_pps : 0.0);

    FILE* tsv = std::fopen("/tmp/net_batch_bench.tsv", "w");
    if (tsv != nullptr) {
        std::fprintf(tsv, "mode\tpackets\tsyscalls\tseconds\tpps\tns_per_packet\tspeedup\n");
        for (const auto& row : rows) {
            std::fprintf(tsv, "%s\t%zu\t%zu\t%.6f\t%.0f\t%.2f\t%.2f\n", row.name.c_str(),
                         row.packets, row.syscalls, row.seconds, row.pps,
                         row.ns_per_packet, row.speedup);
        }
        std::fprintf(tsv, "tcp_sendfile\t%zu\t-\t%.6f\t%.1f\t-\t-\n", kTcpSize,
                     sendfile_seconds, sendfile_mibs);
        std::fprintf(tsv, "tcp_read_send\t%zu\t-\t%.6f\t%.1f\t-\t-\n", kTcpSize,
                     copy_seconds, copy_mibs);
        std::fprintf(tsv, "tcp_send_single\t%zu\t%zu\t%.6f\t%.0f\t%.2f\t1.00\n", kMessages,
                     kMessages, message_single_seconds, message_single_pps,
                     message_single_seconds * 1e9 / kMessages);
        std::fprintf(tsv, "tcp_writev64\t%zu\t%zu\t%.6f\t%.0f\t%.2f\t%.2f\n", kMessages,
                     kMessages / 64, message_batch_seconds, message_batch_pps,
                     message_batch_seconds * 1e9 / kMessages,
                     message_single_pps > 0.0 ? message_batch_pps / message_single_pps : 0.0);
        std::fclose(tsv);
    }
    return 0;
}
