/**
 * @file migrate_main.cpp
 * @brief 迁移快照的极简驱动（供统一 CLI 直接 exec，或按需替换为库调用）。
 *
 * 子命令：
 *
 * ```
 * migrate dump     <快照文件>   # 进程 A：跑起 Fiber，挂起后写快照（带签名）
 * migrate restore  <快照文件>   # 进程 B：校验签名/符号/挂起点，恢复并验证完成
 * migrate verify   <快照文件>   # 只校验格式、签名、符号与挂起点可用性
 * migrate selftest              # SHA-256 / HMAC 测试向量自检
 * ```
 *
 * 未给出命令行参数时回退到环境变量 `MIGRATION_MODE` / `MIGRATION_FILE`。
 *
 * 相关环境变量：
 * - `MIGRATION_SIGNING_KEY` / `MIGRATION_SIGNING_KEY_FILE`：签名密钥（必需）；
 * - `MIGRATION_INSECURE_NO_SIGNATURE=1`：仅测试用，生成未签名快照；
 * - `MIGRATION_MSG1` / `MIGRATION_MSG2`：两条消息（默认 4242 / 7）；
 * - `MIGRATION_DEMO_SO`：用指定 SO 里的 `migration_demo_body` 作为 Fiber 主体。
 *   dump 与 restore 可以指向**不同版本**的 SO（符号/挂起点 id 相同、代码布局
 *   可以不同），用来验证 v3 的 safepoint 恢复。
 */
#include "migration/runtime.hpp"
#include "migration/signature.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// 反例二进制（migrate_alt）与正式工具的内容必须不同，才能得到不同的
// ELF build-id，用来验证「快照拒绝跨可执行文件恢复」。
#if defined(MIGRATION_ALT_BINARY)
constexpr int kBuildTag = 2;
#else
constexpr int kBuildTag = 1;
#endif
volatile int g_build_tag = kBuildTag;

void PrintUsage() {
    std::fprintf(stderr,
                 "用法: migrate <dump|restore|verify> <快照文件>\n"
                 "      migrate selftest\n"
                 "  或设置 MIGRATION_MODE / MIGRATION_FILE 环境变量\n"
                 "  签名: MIGRATION_SIGNING_KEY 或 MIGRATION_SIGNING_KEY_FILE（必需）\n"
                 "  可选 MIGRATION_DEMO_SO 指定 demo 主体 SO（可 dump/restore 用不同版本）\n");
}

int ParseIntEnv(const char* name, int fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return fallback;
    return std::atoi(raw);
}

/// 若设置了 MIGRATION_DEMO_SO，则加载它作为 Fiber 主体。
bool LoadOptionalDemoLibrary(std::string* error) {
    const char* so = std::getenv("MIGRATION_DEMO_SO");
    if (so == nullptr || *so == '\0') return true;
    if (!migration::LoadDemoLibrary(so, error)) {
        *error = std::string("加载 MIGRATION_DEMO_SO=") + so + " 失败: " + *error;
        return false;
    }
    std::printf("已加载 demo SO: %s\n", so);
    return true;
}

int RunSelfTest() {
    std::string detail;
    if (!migration::CryptoSelfTest(&detail)) {
        std::fprintf(stderr, "SHA-256/HMAC 自检失败：\n%s", detail.c_str());
        return 1;
    }
    std::printf("%s", detail.c_str());
    std::printf("SHA-256/HMAC 自检通过\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : std::getenv("MIGRATION_MODE");
    if (mode != nullptr && std::strcmp(mode, "selftest") == 0) {
        return RunSelfTest();
    }
    const char* path = argc > 2 ? argv[2] : std::getenv("MIGRATION_FILE");
    if (mode == nullptr || path == nullptr || *mode == '\0' || *path == '\0') {
        PrintUsage();
        return 2;
    }

    std::string error;
    if (std::strcmp(mode, "dump") == 0) {
        migration::SetProcessRole("A");
        if (!LoadOptionalDemoLibrary(&error)) {
            std::fprintf(stderr, "[A] %s\n", error.c_str());
            return 1;
        }
        if (!migration::DumpDemoToFile(path, &error)) {
            std::fprintf(stderr, "[A] dump 失败: %s\n", error.c_str());
            return 1;
        }
        std::printf("[A] dump 完成: %s\n", path);
        return 0;
    }
    if (std::strcmp(mode, "restore") == 0) {
        migration::SetProcessRole("B");
        if (!LoadOptionalDemoLibrary(&error)) {
            std::fprintf(stderr, "[B] %s\n", error.c_str());
            return 1;
        }
        const int first = ParseIntEnv("MIGRATION_MSG1", 4242);
        const int second = ParseIntEnv("MIGRATION_MSG2", 7);
        std::string trace;
        if (!migration::RestoreDemoFromFile(path, first, second, &error, &trace)) {
            std::fprintf(stderr, "[B] restore 失败: %s\n", error.c_str());
            return 1;
        }
        std::printf("[B] restore 完成: 消息 %d/%d 已送达，Fiber 跨进程继续执行并正常结束\n",
                    first, second);
        return 0;
    }
    if (std::strcmp(mode, "verify") == 0) {
        if (!LoadOptionalDemoLibrary(&error)) {
            std::fprintf(stderr, "verify: %s\n", error.c_str());
            return 1;
        }
        std::string report;
        if (!migration::VerifySnapshotFile(path, &error, &report)) {
            std::fprintf(stderr, "verify 失败: %s\n", error.c_str());
            return 1;
        }
        std::printf("%s", report.c_str());
        return 0;
    }
    PrintUsage();
    return 2;
}
