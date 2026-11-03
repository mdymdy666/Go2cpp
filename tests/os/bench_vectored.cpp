// M10 批量 writev/readv 计时基准（只新增本文件；**不进 CMake**）。
//
// 对比同一份数据（1e6 条 × 8 B = 8,000,000 B）：
//   - 逐条：File::Write / File::Read 各 1e6 次；
//   - 批量：File::WritevAll / File::ReadvAll，每批 1024 条（尾批 576 条）。
// 3 轮，每轮内交替测「逐条 → 批量」，最后取 3 轮中位数与逐轮加速比中位数。
//
// 手工编译（本机构建目录 .build-p1-test-wsl，Release/-O3）。
// 注意：p1 预编译的 libgo2cpp_os.so 早于 WritevAll/ReadvAll，必须直接编 src/os 源：
//   g++ -O3 -DNDEBUG -std=c++17 -Iinclude -Isrc/os tests/os/bench_vectored.cpp
//   src/os/file.cpp src/os/config.cpp -L.build-p1-test-wsl -lgo2cpp_config
//   -lgo2cpp_error -Wl,-rpath,$PWD/.build-p1-test-wsl -o /tmp/bench_vectored
// 运行：
//   taskset -c 2,3 /tmp/bench_vectored /tmp
//
// 口径：tmpfs/ext4 由调用方给出；页缓存热（写只落到 page cache，不 fsync；
// 读之前该文件已经完整写过一遍，属于热读）。读缓冲与数据池均预先 memset，
// 避免首轮 page fault 计入单条/批量其中一侧。
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/os/os.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::os::File;

constexpr std::size_t kRecords = 1000000;
constexpr std::size_t kRecBytes = 8;
constexpr std::size_t kTotalBytes = kRecords * kRecBytes;
constexpr int kBatch = 1024;

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("[bench_vectored] FAIL %s\n", what);
    }
}

double NsPerRecord(Clock::time_point begin, Clock::time_point end) {
    const double ns = std::chrono::duration<double, std::nano>(end - begin).count();
    return ns / static_cast<double>(kRecords);
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::vector<std::uint8_t> MakePool() {
    std::vector<std::uint8_t> pool(kTotalBytes);
    for (std::size_t i = 0; i < pool.size(); ++i) {
        pool[i] = static_cast<std::uint8_t>((i * 131u + 17u) & 0xFFu);
    }
    return pool;
}

// 逐条写 1e6 次，每次 8 B。
double RunWriteSingle(File& file, const std::vector<std::uint8_t>& pool) {
    const auto begin = Clock::now();
    for (std::size_t i = 0; i < kRecords; ++i) {
        const ssize_t n = file.Write(pool.data() + i * kRecBytes, kRecBytes);
        if (n != static_cast<ssize_t>(kRecBytes)) {
            Check(false, "Write short/failed");
            break;
        }
    }
    return NsPerRecord(begin, Clock::now());
}

// 逐条读 1e6 次，每次 8 B。
double RunReadSingle(File& file, std::vector<std::uint8_t>& out) {
    const auto begin = Clock::now();
    for (std::size_t i = 0; i < kRecords; ++i) {
        const ssize_t n = file.Read(out.data() + i * kRecBytes, kRecBytes);
        if (n != static_cast<ssize_t>(kRecBytes)) {
            Check(false, "Read short/failed");
            break;
        }
    }
    return NsPerRecord(begin, Clock::now());
}

// 批量写：每批 1024 条，最后一批 576 条。
double RunWriteBatch(File& file, const std::array<struct iovec, kBatch>& full,
                     const std::vector<struct iovec>& tail) {
    const auto begin = Clock::now();
    std::size_t base = 0;
    while (base + static_cast<std::size_t>(kBatch) <= kRecords) {
        const std::size_t got = file.WritevAll(full.data(), kBatch);
        if (got != static_cast<std::size_t>(kBatch) * kRecBytes) {
            Check(false, "WritevAll short/failed");
            break;
        }
        base += static_cast<std::size_t>(kBatch);
    }
    if (base < kRecords) {
        const std::size_t remain = kRecords - base;
        const std::size_t got = file.WritevAll(tail.data(), static_cast<int>(remain));
        if (got != remain * kRecBytes) {
            Check(false, "WritevAll tail short/failed");
        }
    }
    return NsPerRecord(begin, Clock::now());
}

// 批量读：每批 1024 条，最后一批 576 条。
double RunReadBatch(File& file, const std::array<struct iovec, kBatch>& full,
                    const std::vector<struct iovec>& tail) {
    const auto begin = Clock::now();
    std::size_t base = 0;
    while (base + static_cast<std::size_t>(kBatch) <= kRecords) {
        const ssize_t got = file.ReadvAll(full.data(), kBatch);
        if (got != static_cast<ssize_t>(static_cast<std::size_t>(kBatch) * kRecBytes)) {
            Check(false, "ReadvAll short/failed");
            break;
        }
        base += static_cast<std::size_t>(kBatch);
    }
    if (base < kRecords) {
        const std::size_t remain = kRecords - base;
        const ssize_t got = file.ReadvAll(tail.data(), static_cast<int>(remain));
        if (got != static_cast<ssize_t>(remain * kRecBytes)) {
            Check(false, "ReadvAll tail short/failed");
        }
    }
    return NsPerRecord(begin, Clock::now());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "/tmp";
    const std::string path = dir + "/bench_vectored_m10.bin";

    std::vector<std::uint8_t> pool = MakePool();
    std::vector<std::uint8_t> readback(kTotalBytes, 0);
    std::memset(readback.data(), 0, readback.size());  // 预先触页

    std::array<struct iovec, kBatch> write_full{};
    std::array<struct iovec, kBatch> read_full{};
    for (int k = 0; k < kBatch; ++k) {
        write_full[static_cast<std::size_t>(k)].iov_base =
            pool.data() + static_cast<std::size_t>(k) * kRecBytes;
        write_full[static_cast<std::size_t>(k)].iov_len = kRecBytes;
        read_full[static_cast<std::size_t>(k)].iov_base =
            readback.data() + static_cast<std::size_t>(k) * kRecBytes;
        read_full[static_cast<std::size_t>(k)].iov_len = kRecBytes;
    }
    const std::size_t tail_count = kRecords % static_cast<std::size_t>(kBatch);
    const std::size_t tail_base = kRecords - tail_count;
    std::vector<struct iovec> write_tail(tail_count);
    std::vector<struct iovec> read_tail(tail_count);
    for (std::size_t k = 0; k < tail_count; ++k) {
        write_tail[k].iov_base = pool.data() + (tail_base + k) * kRecBytes;
        write_tail[k].iov_len = kRecBytes;
        read_tail[k].iov_base = readback.data() + (tail_base + k) * kRecBytes;
        read_tail[k].iov_len = kRecBytes;
    }

    std::printf(
        "[bench_vectored] records=%zu rec=%zuB total=%zuB batch=%d tail=%zu "
        "rounds=3 path=%s\n",
        kRecords, kRecBytes, kTotalBytes, kBatch, tail_count, path.c_str());

    std::vector<double> ws, wb, rs, rb, w_ratio, r_ratio;
    for (int round = 0; round < 3; ++round) {
        // 写：逐条
        {
            File file;
            Check(File::Create(path, 0644, &file).ok(), "create (single write)");
            const double t = RunWriteSingle(file, pool);
            ws.push_back(t);
            Check(file.Size() == static_cast<off_t>(kTotalBytes),
                  "size after single write");
            file.Seek(0, SEEK_SET);
            const double tr = RunReadSingle(file, readback);
            rs.push_back(tr);
            Check(std::memcmp(readback.data(), pool.data(), kTotalBytes) == 0,
                  "single read content");
        }
        // 写：批量
        {
            File file;
            Check(File::Create(path, 0644, &file).ok(), "create (batch write)");
            const double t = RunWriteBatch(file, write_full, write_tail);
            wb.push_back(t);
            Check(file.Size() == static_cast<off_t>(kTotalBytes),
                  "size after batch write");
            file.Seek(0, SEEK_SET);
            const double tr = RunReadBatch(file, read_full, read_tail);
            rb.push_back(tr);
            Check(std::memcmp(readback.data(), pool.data(), kTotalBytes) == 0,
                  "batch read content");
        }
        w_ratio.push_back(ws.back() / wb.back());
        r_ratio.push_back(rs.back() / rb.back());
        std::printf(
            "[bench_vectored] round=%d write_single=%.1f ns/rec write_batch=%.1f "
            "ns/rec read_single=%.1f ns/rec read_batch=%.1f ns/rec\n",
            round + 1, ws.back(), wb.back(), rs.back(), rb.back());
    }

    const double mws = Median(ws), mwb = Median(wb);
    const double mrs = Median(rs), mrb = Median(rb);
    const double mwr = Median(w_ratio), mrr = Median(r_ratio);
    const double mib = 1024.0 * 1024.0;
    const double total_mib = static_cast<double>(kTotalBytes) / mib;

    std::printf(
        "[bench_vectored] MEDIAN write: single=%.1f ns/rec (%.1f MiB/s) "
        "batch=%.1f ns/rec (%.1f MiB/s) speedup=%.2fx\n",
        mws, total_mib / (mws * 1e-9 * static_cast<double>(kRecords)), mwb,
        total_mib / (mwb * 1e-9 * static_cast<double>(kRecords)), mwr);
    std::printf(
        "[bench_vectored] MEDIAN read : single=%.1f ns/rec (%.1f MiB/s) "
        "batch=%.1f ns/rec (%.1f MiB/s) speedup=%.2fx\n",
        mrs, total_mib / (mrs * 1e-9 * static_cast<double>(kRecords)), mrb,
        total_mib / (mrb * 1e-9 * static_cast<double>(kRecords)), mrr);
    std::printf("[bench_vectored] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
