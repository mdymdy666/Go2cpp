#pragma once

/**
 * @file uploader.hpp
 * @brief JinShan REQ-011 示例：分片 + 断点续传的云端上传器。
 * @details 上传流程：
 *
 * ```
 * 本地文件 --流式读--> 单分片缓冲 --> SHA-256(分片)
 *                                      |
 *              GET /manifest（拿服务端已收分片）
 *                                      |
 *              逐片 PUT /upload（失败指数退避重试）
 *                                      |
 *              POST /finalize（整体 SHA-256 清单校验）
 * ```
 *
 * 内存上界是「一个分片缓冲 + 一个 SHA-256 上下文」，与文件大小无关；
 * 1 GiB 文件在默认 1 MiB 分片下常驻内存约 1 MiB。
 *
 * 协议（纯文本，测试服务端见 server.cpp）：
 * - `GET  /manifest?sha256=<hex>&size=<n>&chunk_size=<n>&chunks=<n>`
 *   → `chunk_size=..\nsize=..\nchunks=..\nreceived=0,1,2`
 * - `PUT  /upload?sha256=<hex>&index=<i>&size=<n>&chunk_size=<n>&chunks=<n>`
 *   头 `X-Chunk-SHA256: <hex>`，body 为分片字节 → `OK`
 * - `POST /finalize?sha256=<hex>&size=<n>&chunk_size=<n>&chunks=<n>`
 *   → `OK <sha256>` 或 4xx/5xx
 *
 * 约定：全文件 SHA-256 是「内容身份」，服务端按它分目录存分片，因此
 * 同一个文件重复上传天然幂等、可续传。
 */

#include <cstdint>
#include <functional>
#include <string>

namespace cloud {

/// 上传配置；字段在 UploadFile 调用时读取一次。
struct Config {
    /// 服务基地址，如 `http://127.0.0.1:29300`（只支持 http，不支持 https）。
    std::string endpoint{"http://127.0.0.1:29300"};
    /// 分片字节数；1 KiB..1 GiB，最后一片可不足该值。
    std::uint64_t chunk_size{1U << 20};
    /// 单个请求失败后的最大重试次数（不含首次尝试）；0 表示不重试。
    int retries{5};
    /// 首次重试等待毫秒数；之后按 2 倍指数退避，封顶 5 s。
    int retry_backoff_ms{100};
    /// 单次 HTTP 请求超时毫秒数。
    int timeout_ms{30000};
    /// 是否发送分片 SHA-256 头并比对整体校验结果；默认 true（REQ-011 要求）。
    /// 注意：内容身份 SHA-256 始终计算（服务端按它寻址、断点续传依赖它），
    /// 关闭本项只是跳过校验请求，不省这次哈希。
    bool sha256{true};
    /// 是否向 stderr 打印续传/重试细节。
    bool verbose{true};
};

/// 上传结果；成功时 sha256/size/chunks 可用，失败时 error 说明原因。
struct Result {
    bool ok{false};                 ///< 全部分片 + 整体校验通过。
    std::string error;              ///< 失败原因（ok=false 时非空）。
    std::string sha256;             ///< 全文件 SHA-256（64 位小写 hex）。
    std::uint64_t size{0};          ///< 文件字节数。
    std::uint32_t chunks{0};        ///< 分片总数。
    std::uint32_t uploaded{0};      ///< 本次实际 PUT 成功的分片数。
    std::uint32_t resumed{0};       ///< 依据服务端清单跳过的分片数。
    int retries_used{0};            ///< 累计重试次数。
    double seconds{0.0};            ///< 上传耗时（steady_clock）。
};

/**
 * @brief 进度回调：`done` 为已完成字节数，`total` 为文件总字节数。
 * @note 回调在调用 UploadFile 的线程同步执行；从「续传起点」和「全文件完成」
 *       两点各至少触发一次。
 */
using ProgressFn = std::function<void(std::uint64_t done, std::uint64_t total)>;

/**
 * @brief 云端上传器：分片、重试、断点续传、整体清单校验。
 *
 * ## 定位
 * 示例的唯一上传入口；一个实例可重复调用 UploadFile（无跨调用状态）。
 *
 * ## 不变量
 * - 任意时刻只持有 <= chunk_size 字节的分片缓冲，不整文件读入内存；
 * - 失败不抛异常，统一通过 Result 返回；
 * - 续传只信任服务端「已收分片清单」，最终仍由整体 SHA-256 兜底。
 */
class Uploader final {
public:
    explicit Uploader(Config config);

    /**
     * @brief 上传一个本地文件到配置的 endpoint。
     *
     * @param path [in] 本地文件路径；不可为目录，必须可读。
     * @param progress [in] 进度回调，可为空（std::function 空对象）。
     *
     * @return 成功/失败结果；失败时 `error` 说明阶段（扫描、清单、分片、校验）。
     *
     * ## 步骤
     * step1. 打开文件、取大小，流式算全文件 SHA-256（内容身份）；
     * step2. GET /manifest 拿已收分片索引，从断点继续并触发首次进度；
     * step3. 逐片流式读取、算分片 SHA、PUT 上传；可重试错误按指数退避重试；
     * step4. POST /finalize，用服务端重算的整体 SHA 与本地值比对。
     *
     * ## 并发/失败语义
     * - 单线程同步调用；同一 Uploader 可顺序复用，不可并发调用；
     * - 分片请求失败重试耗尽后立即返回，已上传分片保留在服务端，可供下次续传。
     */
    Result UploadFile(const std::string& path, const ProgressFn& progress);

    const Config& config() const noexcept { return m_config; }

private:
    Config m_config;
};

/// 解析 `8M` / `1MiB` / `1048576` 形式的分片大小；失败返回 false。
bool ParseSize(const std::string& text, std::uint64_t* bytes);

}  // namespace cloud
