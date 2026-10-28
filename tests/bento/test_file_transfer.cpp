/**
 * @file test_file_transfer.cpp
 * @brief MOD-28 文件传输测试：SHA-256 向量、帧边界、1MB 传输、断点续传、
 *        校验失败重试、路径逃逸拒绝、空文件、大小上限与原子落盘。
 * @details 覆盖 14 个场景：
 *          1. SHA-256 已知向量（空串/abc/fox）；
 *          2. HEADER/CHUNK/ACK 帧往返与非法帧拒绝；
 *          3. 路径沙箱（`..`、绝对路径、反斜杠、盘符、超长、空）；
 *          4. 符号链接逃逸拒绝；
 *          5. 1MB 文件 16 块传输，内容与摘要一致；
 *          6. 断点续传（5 块后中断，重开从 5×64KB 继续）；
 *          7. 块校验失败重试：3 次可恢复、第 4 次 kRetryExhausted；
 *          8. 空文件（0 块，Finish 直接成功）；
 *          9. 文件大小上限；
 *          10. 接收端路径逃逸拒绝；
 *          11. 整文件摘要不符 → Finish 失败且不留目标文件；
 *          12. 目标已存在：默认拒绝，overwrite 可覆盖；
 *          13. 乱序块拒绝、重复块幂等；
 *          14. 尾块小于 chunk_size 的边界。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "go2cpp/bento/net/file_transfer.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::DecodeFileAck;
using go2cpp::bento::DecodeFileChunk;
using go2cpp::bento::DecodeFileHeader;
using go2cpp::bento::EncodeFileAck;
using go2cpp::bento::EncodeFileChunk;
using go2cpp::bento::EncodeFileHeader;
using go2cpp::bento::FileAck;
using go2cpp::bento::FileAckReason;
using go2cpp::bento::FileChunk;
using go2cpp::bento::FileHash;
using go2cpp::bento::FileHashHex;
using go2cpp::bento::FileHeader;
using go2cpp::bento::FileReceiver;
using go2cpp::bento::FileSender;
using go2cpp::bento::FileTransferConfig;
using go2cpp::bento::FileTransferStatus;
using go2cpp::bento::ResolveTransferPath;
using go2cpp::bento::Sha256Of;

/// 创建唯一的临时目录；失败返回空路径。
std::filesystem::path MakeTempDir(const char* tag) {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int index = 0; index < 100; ++index) {
        const std::filesystem::path dir =
            base / (std::string("bento_") + tag + "_" + std::to_string(::getpid()) + "_" +
                    std::to_string(index));
        std::error_code ec;
        if (std::filesystem::create_directories(dir, ec) && !ec) {
            return dir;
        }
    }
    return {};
}

/// 递归删除临时目录。
void RemoveTree(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

/// 生成确定性的伪随机数据（同一 seed 结果恒定，便于重复测试）。
std::vector<std::uint8_t> MakeData(std::size_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> data(size);
    std::uint32_t state = seed | 1u;
    for (std::uint8_t& byte : data) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return data;
}

/// 写文件（父目录必须已存在）。
void WriteFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
}

/// 读文件全部字节。
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

/// 把发送端剩余块全部灌给接收端；返回首个错误（成功/发完返回 kOk）。
FileTransferStatus TransferAll(FileSender& sender, FileReceiver& receiver, int* chunks) {
    std::vector<std::uint8_t> frame;
    while (true) {
        FileTransferStatus status = sender.BuildNextChunkFrame(&frame);
        if (status == FileTransferStatus::kAlreadyCompleted) {
            return FileTransferStatus::kOk;
        }
        if (status != FileTransferStatus::kOk) {
            return status;
        }
        status = receiver.ApplyChunkFrame(frame.data(), frame.size());
        if (status != FileTransferStatus::kOk) {
            return status;
        }
        if (chunks != nullptr) {
            ++*chunks;
        }
    }
}

/// 场景 1：SHA-256 标准向量。
void TestSha256Vectors() {
    BENTO_CHECK_EQ(FileHashHex(Sha256Of(nullptr, 0)),
                   std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const std::string abc = "abc";
    BENTO_CHECK_EQ(FileHashHex(Sha256Of(reinterpret_cast<const std::uint8_t*>(abc.data()),
                                        abc.size())),
                   std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const std::string fox = "The quick brown fox jumps over the lazy dog";
    BENTO_CHECK_EQ(FileHashHex(Sha256Of(reinterpret_cast<const std::uint8_t*>(fox.data()),
                                        fox.size())),
                   std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
    // 同一数据分两次计算必须一致（覆盖流式路径的确定性）。
    std::vector<std::uint8_t> data = MakeData(100000, 3);
    BENTO_CHECK(Sha256Of(data.data(), data.size()) == Sha256Of(data.data(), data.size()));
}

/// 场景 2：三种帧的往返与非法帧拒绝。
void TestFrameRoundTrip() {
    FileHeader header;
    header.transfer_id = 7;
    header.path = "dir/a.bin";
    header.size = 200000;
    header.chunk_size = 65536;
    header.chunk_count = 4;  // ceil(200000/65536)
    header.sha256 = Sha256Of(nullptr, 0);

    std::vector<std::uint8_t> frame = EncodeFileHeader(header);
    BENTO_CHECK(!frame.empty());
    FileHeader decoded;
    std::string error;
    BENTO_CHECK(DecodeFileHeader(frame.data(), frame.size(), &decoded, &error));
    BENTO_CHECK_EQ(decoded.transfer_id, header.transfer_id);
    BENTO_CHECK_EQ(decoded.path, header.path);
    BENTO_CHECK_EQ(decoded.size, header.size);
    BENTO_CHECK_EQ(decoded.chunk_size, header.chunk_size);
    BENTO_CHECK_EQ(decoded.chunk_count, header.chunk_count);
    BENTO_CHECK(decoded.sha256 == header.sha256);
    BENTO_CHECK(!DecodeFileHeader(frame.data(), frame.size() - 1, &decoded, &error));
    std::vector<std::uint8_t> broken = frame;
    broken[1] = 'X';
    BENTO_CHECK(!DecodeFileHeader(broken.data(), broken.size(), &decoded, &error));

    // 路径含 `..` 不允许编码。
    FileHeader escaped = header;
    escaped.path = "../evil.bin";
    BENTO_CHECK(EncodeFileHeader(escaped).empty());
    // chunk_count 与 size/chunk_size 不一致也不允许。
    FileHeader inconsistent = header;
    inconsistent.chunk_count = 3;
    BENTO_CHECK(EncodeFileHeader(inconsistent).empty());

    FileChunk chunk;
    chunk.transfer_id = 7;
    chunk.index = 2;
    chunk.offset = 2 * 65536;
    chunk.data = MakeData(4096, 11);
    chunk.sha256 = Sha256Of(chunk.data.data(), chunk.data.size());
    frame = EncodeFileChunk(chunk);
    BENTO_CHECK(!frame.empty());
    FileChunk chunk_out;
    BENTO_CHECK(DecodeFileChunk(frame.data(), frame.size(), &chunk_out, &error));
    BENTO_CHECK_EQ(chunk_out.index, chunk.index);
    BENTO_CHECK_EQ(chunk_out.offset, chunk.offset);
    BENTO_CHECK(chunk_out.data == chunk.data);
    BENTO_CHECK(chunk_out.sha256 == chunk.sha256);
    BENTO_CHECK(!DecodeFileChunk(frame.data(), frame.size() - 1, &chunk_out, &error));
    broken = frame;
    broken[20] = 0xFF;  // data_len 改成超大值。
    BENTO_CHECK(!DecodeFileChunk(broken.data(), broken.size(), &chunk_out, &error));

    FileAck ack;
    ack.transfer_id = 7;
    ack.next_index = 3;
    ack.next_offset = 3 * 65536;
    ack.reason = FileAckReason::kContinue;
    frame = EncodeFileAck(ack);
    BENTO_CHECK_EQ(frame.size(), go2cpp::bento::kFileAckFrameBytes);
    FileAck ack_out;
    BENTO_CHECK(DecodeFileAck(frame.data(), frame.size(), &ack_out, &error));
    BENTO_CHECK_EQ(ack_out.transfer_id, ack.transfer_id);
    BENTO_CHECK_EQ(ack_out.next_index, ack.next_index);
    BENTO_CHECK_EQ(ack_out.next_offset, ack.next_offset);
    BENTO_CHECK_EQ(ack_out.reason, ack.reason);
    BENTO_CHECK(!DecodeFileAck(frame.data(), frame.size() - 1, &ack_out, &error));
    broken = frame;
    broken[20] = 5;  // reason 越界。
    BENTO_CHECK(!DecodeFileAck(broken.data(), broken.size(), &ack_out, &error));
}

/// 场景 3：路径沙箱的纯字符串与真实路径校验。
void TestPathSandbox() {
    const std::filesystem::path root = MakeTempDir("ft_path");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    std::string resolved;
    BENTO_CHECK_EQ(ResolveTransferPath(root.string(), "a/b.txt", 1024, &resolved),
                   FileTransferStatus::kOk);
    BENTO_CHECK(!resolved.empty());
    BENTO_CHECK(resolved.find("b.txt") != std::string::npos);
    BENTO_CHECK_EQ(ResolveTransferPath(root.string(), "a//b.txt", 1024, &resolved),
                   FileTransferStatus::kOk);
    BENTO_CHECK_EQ(ResolveTransferPath(root.string(), "./a.txt", 1024, &resolved),
                   FileTransferStatus::kOk);

    for (const char* path : {"../evil.txt", "/etc/passwd", "a/../../evil.txt", "..\\evil.txt",
                             "C:\\evil.txt", "", ".", "a/.."}) {
        BENTO_CHECK_EQ(ResolveTransferPath(root.string(), path, 1024, &resolved),
                       FileTransferStatus::kPathRejected);
    }
    BENTO_CHECK_EQ(ResolveTransferPath(root.string(), std::string(2000, 'a'), 1024, &resolved),
                   FileTransferStatus::kPathRejected);
    RemoveTree(root);
}

/// 场景 4：符号链接逃逸必须被拒绝。
void TestSymlinkEscape() {
    const std::filesystem::path root = MakeTempDir("ft_symlink");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directory_symlink("/etc", root / "link", ec);
    if (!ec) {
        std::string resolved;
        BENTO_CHECK_EQ(ResolveTransferPath(root.string(), "link/passwd", 1024, &resolved),
                       FileTransferStatus::kPathRejected);
    }
    RemoveTree(root);
}

/// 场景 5：1MB 文件 16 块完整传输。
void TestOneMegabyteTransfer() {
    const std::filesystem::path root = MakeTempDir("ft_1m");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "source.bin";
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    const std::vector<std::uint8_t> data = MakeData(1024 * 1024, 42);
    WriteFile(source, data);

    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "sub/data.bin", 42), FileTransferStatus::kOk);
    BENTO_CHECK(sender.opened());
    BENTO_CHECK_EQ(sender.chunk_count(), 16u);
    BENTO_CHECK_EQ(sender.header().size, static_cast<std::uint64_t>(1024 * 1024));
    BENTO_CHECK_EQ(sender.next_index(), 0u);

    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(sender.header(), receive_root.string()),
                   FileTransferStatus::kOk);
    BENTO_CHECK_EQ(receiver.next_index(), 0u);
    BENTO_CHECK_EQ(receiver.resume_offset(), 0u);

    int chunks = 0;
    BENTO_CHECK_EQ(TransferAll(sender, receiver, &chunks), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(chunks, 16);
    BENTO_CHECK_EQ(receiver.next_index(), 16u);
    BENTO_CHECK_EQ(receiver.Finish(), FileTransferStatus::kOk);
    BENTO_CHECK(receiver.completed());
    BENTO_CHECK_EQ(receiver.retries_used(), 0);

    const std::vector<std::uint8_t> received = ReadFile(receive_root / "sub" / "data.bin");
    BENTO_CHECK_EQ(received.size(), data.size());
    BENTO_CHECK(received == data);
    RemoveTree(root);
}

/// 场景 6：断点续传。
void TestResumeTransfer() {
    const std::filesystem::path root = MakeTempDir("ft_resume");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "source.bin";
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    const std::vector<std::uint8_t> data = MakeData(1024 * 1024, 99);
    WriteFile(source, data);

    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "data.bin", 9), FileTransferStatus::kOk);

    // 第一次只接收 5 块，然后让接收端离开作用域（保留 .part）。
    {
        FileReceiver partial;
        BENTO_CHECK_EQ(partial.Open(sender.header(), receive_root.string()),
                       FileTransferStatus::kOk);
        std::vector<std::uint8_t> frame;
        for (int index = 0; index < 5; ++index) {
            BENTO_CHECK_EQ(sender.BuildNextChunkFrame(&frame), FileTransferStatus::kOk);
            BENTO_CHECK_EQ(partial.ApplyChunkFrame(frame.data(), frame.size()),
                           FileTransferStatus::kOk);
        }
        BENTO_CHECK_EQ(partial.next_index(), 5u);
    }

    // 续传：从 .part 的大小恢复到 5 块边界。
    FileReceiver resumed;
    BENTO_CHECK_EQ(resumed.Open(sender.header(), receive_root.string()),
                   FileTransferStatus::kOk);
    BENTO_CHECK_EQ(resumed.resume_offset(), static_cast<std::uint64_t>(5 * 64 * 1024));
    BENTO_CHECK_EQ(resumed.next_index(), 5u);
    BENTO_CHECK_EQ(sender.Seek(resumed.next_offset()), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(sender.next_index(), 5u);

    int chunks = 0;
    BENTO_CHECK_EQ(TransferAll(sender, resumed, &chunks), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(chunks, 11);
    BENTO_CHECK_EQ(resumed.Finish(), FileTransferStatus::kOk);
    BENTO_CHECK(ReadFile(receive_root / "data.bin") == data);
    RemoveTree(root);
}

/// 场景 7：块校验失败的重试与耗尽。
void TestChecksumRetry() {
    const std::filesystem::path root = MakeTempDir("ft_retry");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "source.bin";
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    WriteFile(source, MakeData(300 * 1024, 5));

    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "retry.bin", 5), FileTransferStatus::kOk);
    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(sender.header(), receive_root.string()),
                   FileTransferStatus::kOk);

    std::vector<std::uint8_t> frame;
    BENTO_CHECK_EQ(sender.BuildChunkFrame(0, &frame), FileTransferStatus::kOk);
    FileChunk chunk0;
    BENTO_CHECK(DecodeFileChunk(frame.data(), frame.size(), &chunk0, nullptr));
    FileChunk corrupted = chunk0;
    corrupted.data[0] ^= 0xFF;
    BENTO_CHECK_EQ(receiver.ApplyChunk(corrupted), FileTransferStatus::kChecksumMismatch);
    BENTO_CHECK_EQ(receiver.next_index(), 0u);
    BENTO_CHECK_EQ(receiver.chunk_failures(), 1);
    // 正确块重发成功，失败计数归零。
    BENTO_CHECK_EQ(receiver.ApplyChunk(chunk0), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(receiver.next_index(), 1u);
    BENTO_CHECK_EQ(receiver.chunk_failures(), 0);

    // 块 1 连续失败：前 3 次可重试，第 4 次耗尽（max_retries = 3）。
    BENTO_CHECK_EQ(sender.BuildChunkFrame(1, &frame), FileTransferStatus::kOk);
    FileChunk chunk1;
    BENTO_CHECK(DecodeFileChunk(frame.data(), frame.size(), &chunk1, nullptr));
    FileChunk bad = chunk1;
    bad.data[0] ^= 0x01;
    for (int attempt = 0; attempt < 3; ++attempt) {
        BENTO_CHECK_EQ(receiver.ApplyChunk(bad), FileTransferStatus::kChecksumMismatch);
    }
    BENTO_CHECK_EQ(receiver.ApplyChunk(bad), FileTransferStatus::kRetryExhausted);
    BENTO_CHECK_EQ(receiver.retries_used(), 5);
    BENTO_CHECK_EQ(receiver.next_index(), 1u);
    receiver.Cancel();
    RemoveTree(root);
}

/// 场景 8：空文件。
void TestEmptyFile() {
    const std::filesystem::path root = MakeTempDir("ft_empty");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "empty.bin";
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    WriteFile(source, {});

    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "empty.bin", 1), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(sender.chunk_count(), 0u);
    std::vector<std::uint8_t> frame;
    BENTO_CHECK_EQ(sender.BuildNextChunkFrame(&frame), FileTransferStatus::kAlreadyCompleted);

    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(sender.header(), receive_root.string()),
                   FileTransferStatus::kOk);
    BENTO_CHECK_EQ(receiver.next_index(), 0u);
    BENTO_CHECK_EQ(receiver.Finish(), FileTransferStatus::kOk);
    BENTO_CHECK(receiver.completed());
    BENTO_CHECK(std::filesystem::exists(receive_root / "empty.bin"));
    BENTO_CHECK_EQ(std::filesystem::file_size(receive_root / "empty.bin"),
                   static_cast<std::uintmax_t>(0));
    RemoveTree(root);
}

/// 场景 9：文件大小上限。
void TestTooLarge() {
    const std::filesystem::path root = MakeTempDir("ft_large");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "big.bin";
    WriteFile(source, MakeData(200 * 1024, 13));

    FileTransferConfig config;
    config.max_file_bytes = 4096;
    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "big.bin", 2, config),
                   FileTransferStatus::kTooLarge);

    // 接收端也必须拒绝超限的 header。
    FileHeader header;
    header.transfer_id = 2;
    header.path = "big.bin";
    header.size = 200 * 1024;
    header.chunk_size = 65536;
    header.chunk_count = 4;
    header.sha256 = Sha256Of(nullptr, 0);
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(header, receive_root.string(), config),
                   FileTransferStatus::kTooLarge);
    RemoveTree(root);
}

/// 场景 10：接收端拒绝逃逸路径。
void TestReceiverPathEscape() {
    const std::filesystem::path root = MakeTempDir("ft_escape");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    FileHeader header;
    header.transfer_id = 3;
    header.path = "../evil.bin";
    header.size = 0;
    header.chunk_size = 65536;
    header.chunk_count = 0;
    header.sha256 = Sha256Of(nullptr, 0);
    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(header, root.string()), FileTransferStatus::kPathRejected);

    header.path = "/etc/evil.bin";
    BENTO_CHECK_EQ(receiver.Open(header, root.string()), FileTransferStatus::kPathRejected);

    header.path = "a/../../evil.bin";
    BENTO_CHECK_EQ(receiver.Open(header, root.string()), FileTransferStatus::kPathRejected);
    BENTO_CHECK(!std::filesystem::exists("/etc/evil.bin"));
    RemoveTree(root);
}

/// 场景 11：整文件摘要不符时 Finish 失败且不留目标。
void TestFinishChecksumFailure() {
    const std::filesystem::path root = MakeTempDir("ft_finish");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "source.bin";
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    WriteFile(source, MakeData(128 * 1024, 21));

    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "bad.bin", 4), FileTransferStatus::kOk);
    FileHeader tampered = sender.header();
    tampered.sha256[0] ^= 0xFF;

    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(tampered, receive_root.string()), FileTransferStatus::kOk);
    const std::string part_path = receiver.temp_path();
    int chunks = 0;
    BENTO_CHECK_EQ(TransferAll(sender, receiver, &chunks), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(chunks, 2);
    BENTO_CHECK_EQ(receiver.Finish(), FileTransferStatus::kChecksumMismatch);
    BENTO_CHECK(!std::filesystem::exists(receive_root / "bad.bin"));
    BENTO_CHECK(!std::filesystem::exists(part_path));
    BENTO_CHECK(!receiver.completed());
    RemoveTree(root);
}

/// 场景 12：目标已存在的覆盖语义。
void TestTargetExists() {
    const std::filesystem::path root = MakeTempDir("ft_exists");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "source.bin";
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    const std::vector<std::uint8_t> data = MakeData(4096, 33);
    WriteFile(source, data);
    WriteFile(receive_root / "target.bin", {1, 2, 3});

    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "target.bin", 6), FileTransferStatus::kOk);

    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(sender.header(), receive_root.string()),
                   FileTransferStatus::kTargetExists);

    FileTransferConfig config;
    config.overwrite = true;
    BENTO_CHECK_EQ(receiver.Open(sender.header(), receive_root.string(), config),
                   FileTransferStatus::kOk);
    int chunks = 0;
    BENTO_CHECK_EQ(TransferAll(sender, receiver, &chunks), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(receiver.Finish(), FileTransferStatus::kOk);
    BENTO_CHECK(ReadFile(receive_root / "target.bin") == data);
    RemoveTree(root);
}

/// 场景 13：乱序拒绝与重复块幂等。
void TestSequenceAndDuplicate() {
    const std::filesystem::path root = MakeTempDir("ft_seq");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "source.bin";
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    WriteFile(source, MakeData(200 * 1024, 55));

    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "seq.bin", 8), FileTransferStatus::kOk);
    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(sender.header(), receive_root.string()),
                   FileTransferStatus::kOk);

    std::vector<std::uint8_t> frame;
    BENTO_CHECK_EQ(sender.BuildChunkFrame(1, &frame), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(receiver.ApplyChunkFrame(frame.data(), frame.size()),
                   FileTransferStatus::kSequenceError);

    BENTO_CHECK_EQ(sender.BuildChunkFrame(0, &frame), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(receiver.ApplyChunkFrame(frame.data(), frame.size()), FileTransferStatus::kOk);
    // 同一块重发是幂等的，不算错误也不推进。
    BENTO_CHECK_EQ(receiver.ApplyChunkFrame(frame.data(), frame.size()),
                   FileTransferStatus::kDuplicateChunk);
    BENTO_CHECK_EQ(receiver.next_index(), 1u);
    receiver.Cancel();
    RemoveTree(root);
}

/// 场景 14：尾块小于 chunk_size。
void TestTailChunk() {
    const std::filesystem::path root = MakeTempDir("ft_tail");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path source = root / "source.bin";
    const std::filesystem::path receive_root = root / "recv";
    std::error_code ec;
    std::filesystem::create_directories(receive_root, ec);
    const std::vector<std::uint8_t> data = MakeData(300 * 1024, 71);
    WriteFile(source, data);

    FileSender sender;
    BENTO_CHECK_EQ(sender.Open(source.string(), "tail.bin", 10), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(sender.chunk_count(), 5u);

    std::vector<std::uint8_t> frame;
    BENTO_CHECK_EQ(sender.BuildChunkFrame(4, &frame), FileTransferStatus::kOk);
    FileChunk tail;
    BENTO_CHECK(DecodeFileChunk(frame.data(), frame.size(), &tail, nullptr));
    BENTO_CHECK_EQ(tail.data.size(), static_cast<std::size_t>(300 * 1024 - 4 * 64 * 1024));

    FileReceiver receiver;
    BENTO_CHECK_EQ(receiver.Open(sender.header(), receive_root.string()),
                   FileTransferStatus::kOk);
    int chunks = 0;
    BENTO_CHECK_EQ(TransferAll(sender, receiver, &chunks), FileTransferStatus::kOk);
    BENTO_CHECK_EQ(chunks, 5);
    BENTO_CHECK_EQ(receiver.Finish(), FileTransferStatus::kOk);
    BENTO_CHECK(ReadFile(receive_root / "tail.bin") == data);
    RemoveTree(root);
}

}  // namespace

int main() {
    TestSha256Vectors();
    TestFrameRoundTrip();
    TestPathSandbox();
    TestSymlinkEscape();
    TestOneMegabyteTransfer();
    TestResumeTransfer();
    TestChecksumRetry();
    TestEmptyFile();
    TestTooLarge();
    TestReceiverPathEscape();
    TestFinishChecksumFailure();
    TestTargetExists();
    TestSequenceAndDuplicate();
    TestTailChunk();
    return bento_test::Finish("test_file_transfer");
}
