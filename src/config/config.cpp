/**
 * @file src/config/config.cpp
 * @brief 配置中心的实现：INI 语法、事务式热加载、监听线程与运行时绑定。
 *
 * ## 模块定位
 *
 * 配置中心的**实现层**。上游是 `example/config/`、`tests/config/`、
 * `tests/logging/test_log_config.cpp` 以及 `src/network/hook.cpp` 的
 * `RegisterHookConfig`；下游只使用标准库（`std::filesystem`、`std::thread`、
 * `std::from_chars`），不依赖网络/调度器实现。
 *
 * ## 本文件定义什么
 *
 * - `IniFile::Load/Parse/Get/Has` 与内部 `Trim`/`ParseSize`/`ParseBool`；
 * - 稳定快照读取（`ReadStableFileContents` + `.lock` 协议）与 `FileStamp`；
 * - `RuntimeConfig::FromIni/Validate/ApplyLogging` 与范围常量；
 * - `Config` 的注册、事务提交、监听线程、导出；
 * - `BindLoggingConfig`/`BindRuntimeConfig`/`LoadRuntimeConfig`；
 * - `SplitConfigName`/`JoinConfigName` 与 INI 转义辅助。
 *
 * ## 阅读建议
 *
 * 先读 `ReadStableFileContents`（为什么需要两次元数据+内容比对），再读
 * `Config::LoadFromIni`（预检、跨字段校验、写入、广播四段），最后看
 * `StartWatcher` 的轮询与 `.lock` 处理。事务状态见 `docs/state-machines/Config.md`。
 *
 * ## 边界与陷阱
 *
 * - `IniFile::Parse` 先构造临时表，成功才 swap，语法错误不会留下半解析状态；
 * - `RuntimeConfig::FromIni` 在副本上操作，校验通过才写回，失败不动原对象；
 * - 监听线程吞掉所有监听器异常并继续轮询，避免一次插件错误终止热加载；
 * - `DumpIni` 与 `IniFile::Parse` 共用一套 `\xHH` 值转义（约定见
 *   `EscapeIniValue`），含 `#`/`;`/换行/首尾空格的值可逐字节往返。
 */
#include "go2cpp/config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iostream>
#include <string_view>
#include <thread>

namespace go2cpp::config {
namespace {

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// ---------------------------------------------------------------------------
// INI 值转义：DumpIni 与 Parse 的唯一约定
// ---------------------------------------------------------------------------
// 编码用 `\xHH`（两位十六进制），解码只在遇到完整序列时发生。未编码的
// `#`/`;` 仍按行内注释截断、未编码的 `\n` 仍还原为换行，保持对既有手写
// 配置（例如 go2cpp.ini 的 `format = ...\n`）的兼容。
//
// 为什么不用 `\n`/`\t` 风格的可读转义：这会与手写配置里的 Windows 路径
// 冲突 —— `C:\temp` 会被读成 tab、`\\server\share` 会被读成空格。`\xHH`
// 在配置里几乎不会自然出现，且未知的 `\xZZ` 会被原样保留。

int HexDigitValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

/// 判断 `text[position]` 起是否为完整的 `\xHH` 序列（hex 大小写均可）。
bool IsHexEscapeAt(const std::string& text, std::size_t position) {
    return position + 3 < text.size() && text[position] == '\\' &&
           text[position + 1] == 'x' &&
           HexDigitValue(text[position + 2]) >= 0 &&
           HexDigitValue(text[position + 3]) >= 0;
}

/// 还原 `\xHH`；不完整的 `\xZZ` 或其它反斜杠序列原样保留。
std::string UnescapeIniValue(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t position = 0; position < text.size(); ++position) {
        if (IsHexEscapeAt(text, position)) {
            const int high = HexDigitValue(text[position + 2]);
            const int low = HexDigitValue(text[position + 3]);
            out += static_cast<char>((high << 4) | low);
            position += 3;
            continue;
        }
        out += text[position];
    }
    return out;
}

/**
 * @brief 从 `=` 右侧的原始文本提取值。
 *
 * ## 步骤
 * step1. 找到第一个**未转义**的 `#`/`;` 作为注释起点（扫描时跳过 `\xHH`，
 *        否则 DumpIni 写出的 `\x23` 会被误判成注释）；
 * step2. trim 掉语法空白；
 * step3. 先按旧规则把字面量 `\n` 还原为换行（手写配置兼容），再解码
 *        `\xHH` —— 顺序不能反，否则解码出的反斜杠会被二次替换。
 */
std::string ExtractIniValue(const std::string& raw) {
    std::size_t end = raw.size();
    for (std::size_t position = 0; position < raw.size(); ++position) {
        if (IsHexEscapeAt(raw, position)) {
            position += 3;  // 跳过 \xHH 的 4 个字符；for 的 ++ 再走 1
            continue;
        }
        if (raw[position] == '#' || raw[position] == ';') {
            end = position;
            break;
        }
    }
    std::string value = Trim(raw.substr(0, end));
    // 兼容旧格式：未编码的字面量 `\n` 还原为换行（go2cpp.ini 的日志格式
    // 依赖它）。必须在解码 `\xHH` 之前做，避免把解码出的 `\` + `n` 二次替换。
    for (std::size_t position = 0;
         (position = value.find("\\n", position)) != std::string::npos;) {
        value.replace(position, 2, "\n");
        ++position;
    }
    return UnescapeIniValue(value);
}

bool ParseSize(const std::string& text, std::size_t* value) {
    if (!value) return false;
    const auto clean = Trim(text);
    if (clean.empty()) return false;
    std::size_t parsed = 0;
    const auto result = std::from_chars(clean.data(), clean.data() + clean.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != clean.data() + clean.size()) return false;
    *value = parsed;
    return true;
}

bool ParseBool(const std::string& text, bool* value) {
    if (!value) return false;
    auto clean = Trim(text);
    std::transform(clean.begin(), clean.end(), clean.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (clean == "1" || clean == "true" || clean == "yes" || clean == "on") *value = true;
    else if (clean == "0" || clean == "false" || clean == "no" || clean == "off") *value = false;
    else return false;
    return true;
}

// 配置文件是外部输入。这里集中定义上限，避免某一个模块单独放宽
// 参数后造成过量线程、内存或超长等待。0 只在文档明确说明时表示自动。
constexpr std::size_t kMaxWorkers = 32U;
constexpr std::size_t kMaxLocalQueue = 1U << 20U;
// 与 SchedulerConfig::fiber_stack_size 的新默认值 2048 对齐：Linux 页粒度
// 是 4KiB，归一化后实际初始提交 4096。允许显式配置更小值没有意义，因为
// normalize_stack_size 会把它抬到 1 页；下限取 2048 而不是 16384。
constexpr std::size_t kMinFiberStack = 2048U;
constexpr std::size_t kMaxFiberStack = 64U * 1024U * 1024U;
constexpr std::size_t kMaxAffinityBudget = 1U << 20U;
constexpr auto kMaxDuration = std::chrono::hours(24);
constexpr std::size_t kMaxDurationMilliseconds =
    static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(kMaxDuration).count());

bool ValidateSchedulerSize(const char* key, std::size_t value,
                           std::string* error) {
    const auto fail = [error, key](const std::string& reason) {
        if (error) *error = std::string("scheduler.") + key + reason;
        return false;
    };
    if (std::string_view(key) == "processor_count" ||
        std::string_view(key) == "min_workers" ||
        std::string_view(key) == "max_workers") {
        return value == 0 || value <= kMaxWorkers
                   ? true
                   : fail(" 不能超过 32");
    }
    if (std::string_view(key) == "local_queue_limit") {
        if (value == 0) return fail(" 必须大于 0");
        return value <= kMaxLocalQueue ? true : fail(" 不能超过 1048576");
    }
    if (std::string_view(key) == "fiber_stack_size") {
        if (value == 0) return true;
        if (value < kMinFiberStack) return fail(" 小于 2048 时可能无法建立安全栈");
        return value <= kMaxFiberStack ? true : fail(" 不能超过 67108864");
    }
    if (std::string_view(key) == "task_affinity_budget") {
        return value <= kMaxAffinityBudget ? true : fail(" 不能超过 1048576");
    }
    if (std::string_view(key) == "fiber_bin_capacity") {
        return value <= 4096U ? true : fail(" 不能超过 4096");
    }
    return true;
}

bool ValidateDuration(const char* key, std::size_t value, std::string* error) {
    if (value == 0) {
        if (error) *error = std::string("scheduler.") + key + " 必须大于 0 毫秒";
        return false;
    }
    if (value > kMaxDurationMilliseconds) {
        if (error) *error = std::string("scheduler.") + key + " 不能超过 24 小时";
        return false;
    }
    return true;
}

bool ReadSize(const IniFile& ini, const char* section, const char* key,
              std::size_t* target, std::string* error) {
    if (!ini.Has(section, key)) return true;
    if (!ParseSize(ini.Get(section, key), target)) {
        if (error) *error = std::string("配置项不是非负整数: ") + section + "." + key;
        return false;
    }
    // 静态配置和热更新必须使用同一套范围规则；这样错误会在读取具体字段时
    // 报告，而不是等到调度器已经部分初始化后才发现。
    //
    // 注意：ValidateSchedulerSize 只认识 scheduler 段的键名，其它段（例如
    // timer 的 warn_after_days / error_after_days）传入时既不在它的白名单里
    // 也不该被它拒绝。因此这里必须按段分流：只有 scheduler 段才走那套范围
    // 规则，其它段解析成功即成功。
    //
    // 修复前写成「if (section == "scheduler" && Validate(...)) return true;
    // return false;」——非 scheduler 段会跳过 return true 直落 return false，
    // 导致 [timer] 段的值永远无法被读取，而且失败时不设置 error（静默失败）。
    if (std::string_view(section) == "scheduler") {
        return ValidateSchedulerSize(key, *target, error);
    }
    return true;
}

// 文件被编辑器直接截断并重写时，监听线程可能恰好读到半个文件。用
// “元数据相同 + 内容相同”的两次快照确认稳定状态；确认失败只重试，绝不
// 把半文件提交给配置中心。生产环境仍建议写临时文件后 rename 替换。
struct FileStamp final {
    std::filesystem::file_time_type modified{};
    std::uintmax_t size{0};

    bool operator==(const FileStamp& other) const noexcept {
        return modified == other.modified && size == other.size;
    }
};

bool ReadFileStamp(const std::string& path, FileStamp* stamp,
                   std::string* error) {
    if (!stamp) return false;
    std::error_code ec;
    stamp->modified = std::filesystem::last_write_time(path, ec);
    if (ec) {
        if (error) *error = "无法读取配置文件时间戳: " + path;
        return false;
    }
    stamp->size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (error) *error = "无法读取配置文件大小: " + path;
        return false;
    }
    return true;
}

std::string EditLockPath(const std::string& path) {
    return path + ".lock";
}

// 配置写入方通过 path + ".lock" 声明编辑事务：锁文件非空表示配置仍在
// 修改，空文件或不存在表示可以读取。这里只检查文件大小，不读取锁文件
// 内容，避免监听线程因为锁文件本身被截断而误判为已提交。
bool IsEditLocked(const std::string& path, bool* locked, std::string* error) {
    if (!locked) return false;
    const auto lock_path = EditLockPath(path);
    std::error_code ec;
    const bool exists = std::filesystem::exists(lock_path, ec);
    if (ec) {
        if (error) *error = "无法检查配置编辑锁: " + lock_path;
        return false;
    }
    if (!exists) {
        *locked = false;
        return true;
    }
    const auto size = std::filesystem::file_size(lock_path, ec);
    if (ec) {
        if (error) *error = "无法读取配置编辑锁大小: " + lock_path;
        return false;
    }
    *locked = size != 0;
    return true;
}

bool ReadFileContents(const std::string& path, std::string* contents,
                      std::string* error) {
    if (!contents) return false;
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        if (error) *error = "无法打开配置文件: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        if (error) *error = "读取配置文件失败: " + path;
        return false;
    }
    *contents = buffer.str();
    return true;
}

/**
 * @brief 读取「编辑中不会变化」的配置文件快照，配合 `path + ".lock"` 协议。
 *
 * ## 步骤
 * step1. 最多尝试 8 次，每次失败间隔 2 ms；
 * step2. 每轮先查编辑锁：锁非空说明写入方仍在编辑，等待后重试；
 * step3. 记录 before 时间戳/大小并读第一份内容；
 * step4. 再次记录 after，要求 before == after；读完后再查一次锁；
 * step5. 再读第二份内容与前后时间戳，要求两次元数据一致且内容完全相同；
 * step6. 校验通过才把第二份内容交给调用方；
 * step7. 全部重试失败时区分「锁未清空」与「文件持续变化」给出中文诊断。
 *
 * ## 为什么需要两次快照
 * 编辑器可能直接截断重写；只读一次会拿到半个文件。双快照 + 双锁检查把
 * 「读到半文件」的概率压到可接受范围，生产环境仍建议临时文件 + 原子 rename。
 */
bool ReadStableFileContents(const std::string& path, std::string* contents,
                            std::string* error) {
    // step1: 重试上限；超过后宁可失败也不提交可能不完整的配置
    constexpr int kAttempts = 8;
    constexpr auto kRetryDelay = std::chrono::milliseconds(2);
    std::string last_error;
    bool lock_seen = false;
    // step2: 每轮都重新检查编辑锁，锁释放后允许重试
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        // step3: 锁文件非空 = 写入方仍持有编辑事务
        bool edit_locked = false;
        if (!IsEditLocked(path, &edit_locked, &last_error)) {
            std::this_thread::sleep_for(kRetryDelay);
            continue;
        }
        if (edit_locked) {
            lock_seen = true;
            std::this_thread::sleep_for(kRetryDelay);
            continue;
        }
        // step4: 第一份快照：元数据前后一致 + 内容读成功
        FileStamp before;
        std::string first;
        FileStamp after;
        if (ReadFileStamp(path, &before, &last_error) &&
            ReadFileContents(path, &first, &last_error) &&
            ReadFileStamp(path, &after, &last_error) && before == after) {
            // step5: 读完后锁可能刚被写入方拿到，必须再确认一次
            bool locked_after_read = false;
            if (!IsEditLocked(path, &locked_after_read, &last_error)) {
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            if (locked_after_read) {
                lock_seen = true;
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            // step6: 第二份快照必须与第一份逐字节相同，排除读期间变化
            FileStamp verify_before;
            std::string second;
            FileStamp verify_after;
            if (ReadFileStamp(path, &verify_before, &last_error) &&
                ReadFileContents(path, &second, &last_error) &&
                ReadFileStamp(path, &verify_after, &last_error) &&
                after == verify_before && verify_before == verify_after &&
                first == second) {
                bool locked_after_verify = false;
                if (!IsEditLocked(path, &locked_after_verify, &last_error)) {
                    std::this_thread::sleep_for(kRetryDelay);
                    continue;
                }
                if (locked_after_verify) {
                    lock_seen = true;
                    std::this_thread::sleep_for(kRetryDelay);
                    continue;
                }
                // step7: 只有双快照与双锁检查全部通过才提交内容
                *contents = std::move(second);
                return true;
            }
        }
        std::this_thread::sleep_for(kRetryDelay);
    }
    // step8: 明确告诉调用方是锁未清还是文件仍在变化
    if (error) {
        *error = lock_seen
                     ? "配置文件正在编辑，请先清空编辑锁: " + EditLockPath(path)
                     : "配置文件在读取期间仍在变化，请使用临时文件加 rename 原子替换: " +
                           path;
        if (!last_error.empty()) *error += "（" + last_error + "）";
    }
    return false;
}

}  // namespace

bool IniFile::Load(const std::string& path, std::string* error) {
    std::string contents;
    return ReadStableFileContents(path, &contents, error) &&
           Parse(contents, error);
}

/**
 * @brief 解析内存中的 INI 文本；成功才替换当前节表。
 *
 * ## 步骤
 * step1. 构造临时 `parsed_sections`，任何语法错误都不影响旧内容；
 * step2. 逐行 trim；空行/`#`/`;` 注释跳过；`[...]` 切换当前节；
 * step3. `key=value` 按第一个 `=` 切分，空键名报错；
 * step4. 值交给 `ExtractIniValue`：未转义的 `#`/`;` 截断为注释，字面量
 *        `\n` 还原为换行，`\xHH` 还原为原始字节（与 `DumpIni` 对称）；
 * step5. 完整解析成功后 `swap` 到 `m_sections`。
 *
 * ## 边界
 * `Load` 会先经过稳定快照读取，因此本函数拿到的应是完整文本；直接调用 `Parse`
 * 的调用方需自己保证文本完整性。
 */
bool IniFile::Parse(const std::string& text, std::string* error) {
    // 先构造临时快照，只有完整解析成功才替换当前内容；语法错误不能
    // 让调用方看到“前半段已更新、后半段丢失”的半解析状态。
    // step1: 临时快照，语法错误时旧内容保持不变
    decltype(m_sections) parsed_sections;
    std::string current = "global";
    std::istringstream stream(text);
    std::string line; std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        auto clean = Trim(line);
        if (clean.empty() || clean.front() == '#' || clean.front() == ';') continue;
        // step2: 节标题必须闭合；空节名直接失败
        if (clean.front() == '[') {
            if (clean.back() != ']') { if (error) *error = "节标题缺少 ]，行 " + std::to_string(line_number); return false; }
            current = Trim(clean.substr(1, clean.size() - 2));
            if (current.empty()) { if (error) *error = "节标题为空，行 " + std::to_string(line_number); return false; }
            continue;
        }
        // step3: 只按第一个 `=` 切分，值里允许再出现 `=`
        const auto equal = clean.find('=');
        if (equal == std::string::npos) { if (error) *error = "配置项缺少 =，行 " + std::to_string(line_number); return false; }
        const auto key = Trim(clean.substr(0, equal));
        // step4: 行内注释、去空白、`\n` 与 `\xHH` 的还原都集中在
        // ExtractIniValue，保证与 DumpIni 的转义成对。
        const auto value = ExtractIniValue(clean.substr(equal + 1));
        if (key.empty()) { if (error) *error = "配置键为空，行 " + std::to_string(line_number); return false; }
        parsed_sections[current][key] = value;
    }
    // step5: 全部成功后一次性替换，读者不会看到半解析状态
    m_sections.swap(parsed_sections);
    return true;
}

std::string IniFile::Get(const std::string& section, const std::string& key,
                         std::string fallback) const {
    const auto section_it = m_sections.find(section);
    if (section_it == m_sections.end()) return fallback;
    const auto value_it = section_it->second.find(key);
    return value_it == section_it->second.end() ? fallback : value_it->second;
}

bool IniFile::Has(const std::string& section, const std::string& key) const {
    const auto section_it = m_sections.find(section);
    return section_it != m_sections.end() && section_it->second.find(key) != section_it->second.end();
}

/**
 * @brief 从 INI 构造一份校验通过的 RuntimeConfig 快照。
 *
 * ## 步骤
 * step1. 以传入对象为基准拷贝到 `result`，失败时不污染原对象；
 * step2. 读取 `scheduler.*` 的尺寸/时长/布尔项；尺寸走统一范围规则；
 * step3. 读取 `log.*` 级别、stdout 与文本字段；
 * step4. 读取 `timer.*` 两个天数阈值；
 * step5. 调用 `Validate()` 做跨字段校验（min/max、warn/error、空格式串等）；
 * step6. 全部通过后才 `*config = std::move(result)`。
 */
bool RuntimeConfig::FromIni(const IniFile& ini, RuntimeConfig* config, std::string* error) {
    if (!config) { if (error) *error = "RuntimeConfig 输出指针为空"; return false; }
    // step1: 在副本上构造；任何失败都不改动调用方对象
    RuntimeConfig result = *config;
    if (!ReadSize(ini, "scheduler", "processor_count", &result.scheduler.processor_count, error) ||
        !ReadSize(ini, "scheduler", "max_workers", &result.scheduler.max_workers, error) ||
        !ReadSize(ini, "scheduler", "min_workers", &result.scheduler.min_workers, error) ||
        !ReadSize(ini, "scheduler", "local_queue_limit", &result.scheduler.local_queue_limit, error) ||
        !ReadSize(ini, "scheduler", "fiber_stack_size", &result.scheduler.fiber_stack_size, error) ||
        !ReadSize(ini, "scheduler", "task_affinity_budget", &result.scheduler.task_affinity_budget, error) ||
        !ReadSize(ini, "scheduler", "fiber_bin_capacity", &result.scheduler.fiber_bin_capacity, error)) return false;
    // step2: 时长项先按毫秒读取，再走 ValidateDuration 的 24 小时上限
    std::size_t value = 0;
    if (ini.Has("scheduler", "idle_wait_ms")) { if (!ReadSize(ini, "scheduler", "idle_wait_ms", &value, error) || !ValidateDuration("idle_wait_ms", value, error)) return false; result.scheduler.idle_wait = std::chrono::milliseconds(value); }
    if (ini.Has("scheduler", "idle_worker_timeout_ms")) { if (!ReadSize(ini, "scheduler", "idle_worker_timeout_ms", &value, error) || !ValidateDuration("idle_worker_timeout_ms", value, error)) return false; result.scheduler.idle_worker_timeout = std::chrono::milliseconds(value); }
    if (ini.Has("scheduler", "sysmon_interval_ms")) { if (!ReadSize(ini, "scheduler", "sysmon_interval_ms", &value, error) || !ValidateDuration("sysmon_interval_ms", value, error)) return false; result.scheduler.sysmon_interval = std::chrono::milliseconds(value); }
    if (ini.Has("scheduler", "long_syscall_threshold_ms")) { if (!ReadSize(ini, "scheduler", "long_syscall_threshold_ms", &value, error) || !ValidateDuration("long_syscall_threshold_ms", value, error)) return false; result.scheduler.long_syscall_threshold = std::chrono::milliseconds(value); }
    if (ini.Has("scheduler", "allow_worker_oversubscription") && !ParseBool(ini.Get("scheduler", "allow_worker_oversubscription"), &result.scheduler.allow_worker_oversubscription)) { if (error) *error = "allow_worker_oversubscription 不是布尔值"; return false; }
    if (ini.Has("scheduler", "enable_sysmon") && !ParseBool(ini.Get("scheduler", "enable_sysmon"), &result.scheduler.enable_sysmon)) { if (error) *error = "enable_sysmon 不是布尔值"; return false; }
    if (ini.Has("scheduler", "pin_workers_to_cpu") && !ParseBool(ini.Get("scheduler", "pin_workers_to_cpu"), &result.scheduler.pin_workers_to_cpu)) { if (error) *error = "pin_workers_to_cpu 不是布尔值"; return false; }
    if (ini.Has("scheduler", "collect_metrics") && !ParseBool(ini.Get("scheduler", "collect_metrics"), &result.scheduler.collect_metrics)) { if (error) *error = "collect_metrics 不是布尔值"; return false; }
    // step3: 日志字段允许缺省，缺省时沿用调用方当前值
    if (ini.Has("log", "level") && !log::ParseLevel(ini.Get("log", "level"), &result.log_level)) { if (error) *error = "log.level 不是有效级别"; return false; }
    if (ini.Has("log", "stdout") && !ParseBool(ini.Get("log", "stdout"), &result.log_stdout)) { if (error) *error = "log.stdout 不是布尔值"; return false; }
    // step4: 文本字段不做 trim 之外的改写，空值留给 Validate 统一拒绝
    result.log_directory = ini.Get("log", "directory", result.log_directory);
    result.log_file = ini.Get("log", "file", result.log_file);
    result.log_format = ini.Get("log", "format", result.log_format);
    // step5: timer 段只接受非负整数，跨字段关系在 Validate 中检查
    if (ini.Has("timer", "warn_after_days") &&
        !ReadSize(ini, "timer", "warn_after_days", &result.timer_warn_after_days, error)) return false;
    if (ini.Has("timer", "error_after_days") &&
        !ReadSize(ini, "timer", "error_after_days", &result.timer_error_after_days, error)) return false;
    // step6: 校验先于写回，保证对象要么全旧要么全新
    if (!result.Validate(error)) return false;
    *config = std::move(result);
    return true;
}

/**
 * @brief 统一校验调度器/日志/定时器参数边界。
 *
 * @details 这里是配置文件与启动路径的**最后一道闸**：线程数上限、队列上限、
 * Fiber 栈范围、四个时长上限、日志目录/文件名/格式非空、timer 阈值关系都在
 * 此集中判断。错误信息带字段名，便于直接打印给用户。
 * @return 全部合法返回 true；否则写 `error`（可为空）并返回 false。
 */
bool RuntimeConfig::Validate(std::string* error) const {
    const auto fail = [error](const std::string& message) { if (error) *error = message; return false; };
    const auto& value = scheduler;
    if (value.max_workers > kMaxWorkers) return fail("scheduler.max_workers 不能超过 32");
    if (value.max_workers != 0 && value.min_workers != 0 && value.max_workers < value.min_workers) return fail("scheduler.max_workers 不能小于 min_workers");
    if (value.processor_count > kMaxWorkers) return fail("scheduler.processor_count 不能超过 32");
    if (value.local_queue_limit == 0) return fail("scheduler.local_queue_limit 必须大于 0");
    if (value.local_queue_limit > kMaxLocalQueue) return fail("scheduler.local_queue_limit 不能超过 1048576");
    if (value.fiber_stack_size != 0 &&
        (value.fiber_stack_size < kMinFiberStack || value.fiber_stack_size > kMaxFiberStack)) {
        return fail("scheduler.fiber_stack_size 必须为 0 或处于 2048..67108864 字节");
    }
    if (value.task_affinity_budget > kMaxAffinityBudget) return fail("scheduler.task_affinity_budget 不能超过 1048576");
    if (value.fiber_bin_capacity > 4096) return fail("scheduler.fiber_bin_capacity 不能超过 4096");
    if (value.idle_wait <= std::chrono::milliseconds::zero() || value.idle_wait > kMaxDuration) return fail("scheduler.idle_wait_ms 必须处于 1 毫秒到 24 小时");
    if (value.idle_worker_timeout <= std::chrono::milliseconds::zero() || value.idle_worker_timeout > kMaxDuration) return fail("scheduler.idle_worker_timeout_ms 必须处于 1 毫秒到 24 小时");
    if (value.sysmon_interval <= std::chrono::milliseconds::zero() || value.sysmon_interval > kMaxDuration) return fail("scheduler.sysmon_interval_ms 必须处于 1 毫秒到 24 小时");
    if (value.long_syscall_threshold <= std::chrono::milliseconds::zero() || value.long_syscall_threshold > kMaxDuration) return fail("scheduler.long_syscall_threshold_ms 必须处于 1 毫秒到 24 小时");
    if (log_format.empty()) return fail("log.format 不能为空");
    if (log_directory.empty() || log_file.empty()) return fail("日志目录和文件名不能为空");
    if (timer_warn_after_days != 0 && timer_error_after_days != 0 &&
        timer_warn_after_days > timer_error_after_days) {
        return fail("timer.warn_after_days 不能大于 timer.error_after_days");
    }
    if (timer_warn_after_days > 36500 || timer_error_after_days > 36500) {
        return fail("timer 时长阈值不能超过 36500 天");
    }
    return true;
}

bool RuntimeConfig::ApplyLogging(std::string* error) const {
    if (!Validate(error)) return false;
    log::LoggerManager::Instance().Configure(log_level, log_stdout, log_directory, log_file, log_format);
    return true;
}

bool LoadRuntimeConfig(const std::string& path, RuntimeConfig* config, std::string* error) {
    IniFile ini;
    return ini.Load(path, error) && RuntimeConfig::FromIni(ini, config, error);
}

Config& Config::Instance() {
    static Config instance;
    return instance;
}

Config::~Config() { StopWatcher(); }

ConfigVarBase::ptr Config::LookupBase(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_vars.find(name);
    return it == m_vars.end() ? nullptr : it->second;
}

/**
 * @brief 把 INI 中的键批量应用到已注册变量；两阶段提交。
 *
 * ## 步骤
 * step1. 取得事务锁，保证预检+写入不被另一个 `LoadFromIni` 交错；
 * step2. 快照全部变量，构造候选值表（未出现的键用当前值）；
 * step3. 逐变量做单字段 `ValidateString`，收集待应用列表；
 * step4. 用完整候选快照跑跨字段事务校验器（任何写入之前）；
 * step5. 逐变量 `ApplyString` 写入但不通知；任一失败则逆序 `RollbackPending`；
 * step6. 释放事务锁（监听器可以再次读/提交配置）；
 * step7. 统一 `NotifyPending`，让模块看到完整的新配置；
 * step8. 执行提交回调；单个回调异常不影响已提交的值。
 *
 * ## 失败语义
 * 预检/事务校验失败时没有任何变量被写入；`ApplyString` 失败时已写变量全部回滚。
 * 通知阶段即使回调抛异常也不回滚（配置值已经对外可见）。
 */
bool Config::LoadFromIni(const IniFile& ini, std::string* error) {
    // 预检和提交必须处于同一事务临界区。否则两个监听线程可能分别
    // 通过预检，随后交错 ApplyString，破坏“整批配置一次发布”的约定。
    // step1: 事务锁覆盖预检与写入，通知阶段会提前释放
    std::unique_lock<std::mutex> transaction_lock(m_transaction_mutex);
    // step2: 先取变量快照，避免持锁遍历时被 Lookup/Remove 改变
    const auto variables = List();
    // 先检查所有已注册项对应的文本能否转换；实际写入仍按变量顺序完成，
    // 这样错误能指出具体变量，同时不要求所有类型都暴露内部临时值。
    // step3: pending 保存「变量 + 待应用文本」，顺序即写入顺序
    std::vector<std::pair<ConfigVarBase::ptr, std::string>> pending;
    std::map<std::string, std::string> candidates;
    for (const auto& variable : variables) {
        if (!variable) continue;
        candidates.emplace(variable->name(), variable->ToString());
        const auto separator = variable->name().find('.');
        const auto section = separator == std::string::npos ? "global" : variable->name().substr(0, separator);
        const auto key = separator == std::string::npos ? variable->name() : variable->name().substr(separator + 1);
        if (!ini.Has(section, key)) continue;
        const auto text = ini.Get(section, key);
        if (!variable->ValidateString(text, error)) return false;
        candidates[variable->name()] = text;
        pending.emplace_back(variable, text);
    }
    // 单字段校验只能检查类型和取值范围，跨字段关系必须基于本次文件的
    // 完整候选快照判断。校验发生在任何 ApplyString 之前，失败时不留下半套值。
    // step4: 跨字段校验基于候选快照，必须在任何 ApplyString 之前
    std::map<std::uint64_t, TransactionValidator> validators;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        validators = m_transaction_validators;
    }
    for (const auto& [id, validator] : validators) {
        (void)id;
        if (validator && !validator(candidates, error)) return false;
    }
    // 第二阶段只写入变量，不触发监听器。这样 scheduler/log 等模块不会
    // 看到“只更新了一半”的配置组合；全部变量写入成功后再统一广播。
    // step5: applied 兼作回滚栈；只记录真正产生 pending 的变量
    std::vector<ConfigVarBase::ptr> applied;
    applied.reserve(pending.size());
    try {
        for (const auto& [variable, text] : pending) {
            if (!variable->ApplyString(text, error)) {
                for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
                    (*it)->RollbackPending();
                }
                return false;
            }
            if (variable->HasPending()) applied.push_back(variable);
        }
        // 所有变量已经完成第一阶段提交；通知阶段不持有事务锁，监听器可以
        // 查询注册表或触发下一次配置加载，避免插件回调与事务锁形成死锁。
        // step6: 释放事务锁后再广播，允许监听器再次提交配置
        transaction_lock.unlock();
        // 监听器读取其他变量时只能看到本次文件提交后的值，不会读到本次
        // ApplyString 顺序造成的半套状态。并发的新事务可能在此之后开始，
        // 这是有意的：配置值已经完整发布，回调不阻塞配置刷新线程。
        // step7: 所有变量已写入，监听器只能看到完整的新值集合
        for (const auto& variable : applied) variable->NotifyPending();
        // step8: 提交回调属于扩展点，逐个隔离异常
        if (!applied.empty()) {
            std::map<std::uint64_t, CommitListener> listeners;
            try {
                std::lock_guard<std::mutex> lock(m_mutex);
                listeners = m_commit_listeners;
            } catch (...) {
                // 仅影响提交后的插件通知，不影响已经提交的配置值。
                listeners.clear();
            }
            for (const auto& [id, listener] : listeners) {
                (void)id;
                if (!listener) continue;
                try {
                    listener(applied);
                } catch (...) {
                    // 配置插件只能观察已提交快照；单个插件失败时继续
                    // 执行其余插件，绝不能让配置监听线程退出。
                }
            }
        }
        return true;
    } catch (...) {
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
            (*it)->RollbackPending();
        }
        throw;
    }
}

bool Config::LoadFromFile(const std::string& path, std::string* error) {
    IniFile ini;
    return ini.Load(path, error) && LoadFromIni(ini, error);
}

std::vector<ConfigVarBase::ptr> Config::List() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConfigVarBase::ptr> result;
    result.reserve(m_vars.size());
    for (const auto& [name, variable] : m_vars) result.push_back(variable);
    return result;
}

/// 函数功能：注册配置事务提交后的扩展回调。
/// 执行流程：
/// 1. 校验回调对象；
/// 2. 加锁复制并登记回调；
/// 3. 返回稳定的监听器 ID，供调用方稍后注销。
/// @param[in] listener 事务完成后接收变更变量列表的回调。
/// @return 可传给 DelCommitListener 的唯一 ID。
std::uint64_t Config::AddCommitListener(CommitListener listener) {
    const auto id = m_next_commit_listener.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commit_listeners.emplace(id, std::move(listener));
    return id;
}

/// 函数功能：注销配置事务提交回调。
/// 执行流程：获取注册表锁并删除指定 ID；不存在时保持幂等。
/// @param[in] id AddCommitListener 返回的监听器 ID。
/// @return 无返回值。
void Config::DelCommitListener(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commit_listeners.erase(id);
}

/// 函数功能：注册配置文件事务候选快照校验器。
/// @param[in] validator 接收本次候选键值并返回校验结果的回调。
/// @return 可传给 DelTransactionValidator 的唯一 ID。
std::uint64_t Config::AddTransactionValidator(TransactionValidator validator) {
    const auto id = m_next_transaction_validator.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transaction_validators.emplace(id, std::move(validator));
    return id;
}

/// 函数功能：删除配置文件事务候选快照校验器。
/// @param[in] id AddTransactionValidator 返回的校验器 ID。
/// @return 无返回值；不存在的 ID 按幂等方式处理。
void Config::DelTransactionValidator(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transaction_validators.erase(id);
}

/**
 * @brief 立即加载文件，并启动按周期轮询的后台监听线程。
 *
 * ## 步骤
 * step1. 校验路径/周期，取生命周期锁并先停掉旧线程；
 * step2. 启动前检查 `.lock`：非空说明文件正在编辑，拒绝启动；
 * step3. 记录初始时间戳并做一次同步加载（失败不启动线程）；
 * step4. 置 `m_watching` 后创建线程；
 * step5. 线程用条件变量按 interval 等待，停止时可被立即唤醒；
 * step6. 每轮检查编辑锁；锁非空时标记 retry_pending 并跳过；
 * step7. 时间戳/大小未变化且没有待重试时跳过；
 * step8. 变化则 `LoadFromFile`；成功更新基准，失败/异常打印并保持重试。
 *
 * ## 为什么不只用 mtime
 * 编辑器可能做同尺寸原子替换，mtime 也可能因精度相同而不变；`retry_pending`
 * 在「锁从非空变空」时强制再试一次，覆盖这两种情况。
 */
bool Config::StartWatcher(const std::string& path, std::chrono::milliseconds interval, std::string* error) {
    if (path.empty() || interval <= std::chrono::milliseconds::zero()) {
        if (error) *error = "配置监听路径或周期无效";
        return false;
    }
    // step1: 生命周期锁保护 std::thread 对象；先确保旧线程已退出
    std::lock_guard<std::mutex> lifecycle_lock(m_watcher_lifecycle_mutex);
    stop_watcher_locked();
    // step2: 启动前就有编辑锁时直接失败，避免把半文件当作初始配置
    bool edit_locked = false;
    if (!IsEditLocked(path, &edit_locked, error)) return false;
    if (edit_locked) {
        if (error) {
            *error = "配置文件正在编辑，请先清空编辑锁: " + EditLockPath(path);
        }
        return false;
    }
    FileStamp initial_stamp;
    if (!ReadFileStamp(path, &initial_stamp, error)) {
        return false;
    }
    // step3: 初始加载失败不启动线程；调用方会看到具体错误
    if (!LoadFromFile(path, error)) return false;
    m_watching.store(true, std::memory_order_release);
    // step4: m_watching 先置位，线程启动后即可看到运行状态
    m_watcher = std::thread([this, path, interval, initial_stamp]() mutable {
        auto stamp = initial_stamp;
        // 锁从非空变为空时必须强制尝试一次，即使文件系统的时间戳和大小
        // 没有变化；这样同尺寸原子替换也不会被旧基准短路。
        bool retry_pending = false;
        while (m_watching.load(std::memory_order_acquire)) {
            // 条件变量让停止监听操作立即唤醒，不必等待一个完整刷新周期。
            std::unique_lock<std::mutex> wait_lock(m_watcher_mutex);
            // step5: 条件变量让 StopWatcher 立即唤醒，不必等满一个周期
            m_watcher_cv.wait_for(wait_lock, interval, [this] {
                return !m_watching.load(std::memory_order_acquire);
            });
            wait_lock.unlock();
            if (!m_watching.load(std::memory_order_acquire)) break;
            // step6: 锁非空时不能读文件；置 retry_pending 保证锁释放后会重试
            bool edit_locked = false;
            std::string lock_error;
            if (!IsEditLocked(path, &edit_locked, &lock_error)) {
                std::cerr << "Go2Cpp 配置编辑锁检查失败: " << lock_error << '\n';
                continue;
            }
            if (edit_locked) {
                retry_pending = true;
                continue;
            }
            // step7: 同尺寸原子替换可能不改变 mtime，靠 retry_pending 兜底
            FileStamp current;
            std::string stamp_error;
            if (!ReadFileStamp(path, &current, &stamp_error) ||
                (current == stamp && !retry_pending)) {
                continue;
            }
            std::string error;
            // step8: 监听器异常不能终止监听线程，记录后继续下一轮
            try {
                if (LoadFromFile(path, &error)) {
                    stamp = current;
                    retry_pending = false;
                } else {
                    std::cerr << "Go2Cpp 配置热加载失败: " << error << '\n';
                    retry_pending = true;
                }
            } catch (const std::exception& exception) {
                // 监听器属于用户扩展点，异常不能逃出配置监听线程；
                // 记录本次失败后继续监视后续文件变化。
                std::cerr << "Go2Cpp 配置监听器异常: " << exception.what()
                          << '\n';
                retry_pending = true;
            } catch (...) {
                std::cerr << "Go2Cpp 配置监听器抛出未知异常\n";
                retry_pending = true;
            }
        }
    });
    return true;
}

void Config::stop_watcher_locked() noexcept {
    m_watching.store(false, std::memory_order_release);
    m_watcher_cv.notify_all();
    if (m_watcher.joinable()) {
        if (m_watcher.get_id() == std::this_thread::get_id()) m_watcher.detach();
        else m_watcher.join();
    }
}

void Config::StopWatcher() {
    std::lock_guard<std::mutex> lifecycle_lock(m_watcher_lifecycle_mutex);
    stop_watcher_locked();
}

/**
 * @brief 把 `log.*` 注册为配置变量，并让任意一项变化都重建 LoggerManager 配置。
 *
 * @details 用 `weak_ptr` 捕获配置变量，避免 `ConfigVar -> Listener -> ConfigVar`
 * 引用环；`apply` 每次读取全部五项再调用一次 `Configure`，避免逐项更新造成
 * 「新级别 + 旧格式」的中间态。
 * @return 全部变量注册成功返回 true；类型冲突写 error 并返回 false。
 */
bool BindLoggingConfig(Config& config, std::string* error) {
    auto level = config.Lookup<log::Level>("log.level", log::Level::Warn, "日志最低级别");
    auto stdout_enabled = config.Lookup<bool>("log.stdout", false, "是否输出到 stdout");
    auto directory = config.Lookup<std::string>("log.directory", "log", "日志目录");
    auto file = config.Lookup<std::string>("log.file", "go2cpp.log", "日志文件名");
    auto format = config.Lookup<std::string>("log.format", "{time} [{level}] {logger} ({file}:{line}) {message}\n", "日志格式");
    if (!level || !stdout_enabled || !directory || !file || !format) {
        if (error) *error = "日志配置变量类型冲突";
        return false;
    }
    const std::weak_ptr<ConfigVar<log::Level>> weak_level = level;
    const std::weak_ptr<ConfigVar<bool>> weak_stdout = stdout_enabled;
    const std::weak_ptr<ConfigVar<std::string>> weak_directory = directory;
    const std::weak_ptr<ConfigVar<std::string>> weak_file = file;
    const std::weak_ptr<ConfigVar<std::string>> weak_format = format;
    const auto apply = [weak_level, weak_stdout, weak_directory, weak_file, weak_format]() {
        const auto current_level = weak_level.lock();
        const auto current_stdout = weak_stdout.lock();
        const auto current_directory = weak_directory.lock();
        const auto current_file = weak_file.lock();
        const auto current_format = weak_format.lock();
        if (!current_level || !current_stdout || !current_directory || !current_file || !current_format) return;
        log::LoggerManager::Instance().Configure(current_level->GetValue(), current_stdout->GetValue(),
                                                  current_directory->GetValue(), current_file->GetValue(), current_format->GetValue());
    };
    level->AddListener([apply](const log::Level&, const log::Level&) { apply(); });
    stdout_enabled->AddListener([apply](const bool&, const bool&) { apply(); });
    directory->AddListener([apply](const std::string&, const std::string&) { apply(); });
    file->AddListener([apply](const std::string&, const std::string&) { apply(); });
    format->AddListener([apply](const std::string&, const std::string&) { apply(); });
    apply();
    return true;
}

/**
 * @brief 把 `scheduler.*`/`timer.*` 配置项绑定到 `RuntimeConfig` 目标。
 *
 * ## 步骤
 * step1. 先校验目标当前值，避免把监听器装到非法快照上；
 * step2. 绑定日志字段（复用 `BindLoggingConfig`）；
 * step3. 逐个绑定尺寸字段：`Lookup<size_t>` + 范围校验器 + 写回监听器；
 * step4. 绑定 `fiber_bin_capacity`；
 * step5. 绑定三个布尔开关；
 * step6. 绑定四个毫秒时长字段，校验器检查 1 ms..24 h；
 * step7. 绑定 `collect_metrics`；
 * step8. 绑定 timer 两个天数阈值，并安装单值与候选快照两类校验器；
 * step9. 任一 `Lookup` 类型冲突则整体返回 false（已装的监听器保留）。
 *
 * ## 生命周期
 * `target` 必须比所有监听器活得久；监听器用裸指针捕获目标字段。
 */
bool BindRuntimeConfig(Config& config, RuntimeConfig* target, std::string* error) {
    if (!target) { if (error) *error = "RuntimeConfig 绑定目标为空"; return false; }
    // step1: 目标快照非法时直接失败，避免监听器把运行态改得更乱
    if (!target->Validate(error)) return false;
    // step2: 日志字段沿用同一套绑定逻辑，避免两处规则漂移
    if (!BindLoggingConfig(config, error)) return false;
    bool valid = true;
    // step3: 尺寸字段共用同一套范围规则，避免各字段边界漂移
    const auto bind_size = [&config, target, &valid](const std::string& name, std::size_t* field, const char* description) {
        auto variable = config.Lookup<std::size_t>(name, *field, description);
        if (!variable) { valid = false; return; }
        const auto separator = name.find('.');
        const auto key = separator == std::string::npos ? name : name.substr(separator + 1);
        variable->SetValidator([key, target](const std::size_t& value, std::string* validation_error) {
            if (!ValidateSchedulerSize(key.c_str(), value, validation_error)) return false;
            // 在线修改 min/max 时按当前另一侧的值做单步校验，避免监听器
            // 将 RuntimeConfig 留在不可启动的状态。需要同时调整两者时，
            // 先把 max_workers 设为 0（自动），再设置目标值。
            if (key == "min_workers" && value != 0 &&
                target->scheduler.max_workers != 0 &&
                value > target->scheduler.max_workers) {
                if (validation_error) *validation_error =
                    "scheduler.min_workers 不能大于当前 max_workers";
                return false;
            }
            if (key == "max_workers" && value != 0 &&
                target->scheduler.min_workers != 0 &&
                value < target->scheduler.min_workers) {
                if (validation_error) *validation_error =
                    "scheduler.max_workers 不能小于当前 min_workers";
                return false;
            }
            return true;
        });
        variable->AddListener([field](const std::size_t&, const std::size_t& value) { *field = value; });
    };
    // 每个字段一个校验器 + 一个写回监听器
    bind_size("scheduler.processor_count", &target->scheduler.processor_count, "P 数量，0 为自动");
    bind_size("scheduler.min_workers", &target->scheduler.min_workers, "最小 M 数量");
    bind_size("scheduler.max_workers", &target->scheduler.max_workers, "最大 M 数量");
    bind_size("scheduler.local_queue_limit", &target->scheduler.local_queue_limit, "P 本地队列容量");
    bind_size("scheduler.fiber_stack_size", &target->scheduler.fiber_stack_size, "Fiber 初始栈大小");
    bind_size("scheduler.task_affinity_budget", &target->scheduler.task_affinity_budget, "任务亲和预算");
    // step4: fiber_bin_capacity 上限 4096，与 ValidateSchedulerSize 保持一致
    auto fiber_bin_capacity = config.Lookup<std::size_t>(
        "scheduler.fiber_bin_capacity", target->scheduler.fiber_bin_capacity,
        "每个 M 的 FiberBin 容量，最多 4096");
    if (!fiber_bin_capacity) {
        valid = false;
    } else {
        fiber_bin_capacity->SetValidator([](const std::size_t& value, std::string* validation_error) {
            return ValidateSchedulerSize("fiber_bin_capacity", value, validation_error);
        });
        fiber_bin_capacity->AddListener(
            [target](const std::size_t&, const std::size_t& value) {
                target->scheduler.fiber_bin_capacity = value;
            });
    }
    // step5: 三个布尔开关只做写入，范围由 bool 类型本身保证
    auto allow_oversubscription = config.Lookup<bool>("scheduler.allow_worker_oversubscription", target->scheduler.allow_worker_oversubscription, "允许 M 超过 P");
    auto enable_sysmon = config.Lookup<bool>("scheduler.enable_sysmon", target->scheduler.enable_sysmon, "启用 sysmon");
    auto pin_workers = config.Lookup<bool>("scheduler.pin_workers_to_cpu", target->scheduler.pin_workers_to_cpu, "绑定 CPU");
    if (!allow_oversubscription || !enable_sysmon || !pin_workers) valid = false;
    if (allow_oversubscription) {
        allow_oversubscription->AddListener([target](const bool&, const bool& value) {
            target->scheduler.allow_worker_oversubscription = value;
        });
    }
    if (enable_sysmon) {
        enable_sysmon->AddListener([target](const bool&, const bool& value) {
            target->scheduler.enable_sysmon = value;
        });
    }
    if (pin_workers) {
        pin_workers->AddListener([target](const bool&, const bool& value) {
            target->scheduler.pin_workers_to_cpu = value;
        });
    }
    // step6: 时长字段存毫秒，监听器转换为 chrono::milliseconds
    auto idle_wait = config.Lookup<std::size_t>("scheduler.idle_wait_ms", target->scheduler.idle_wait.count(), "空闲等待毫秒");
    auto idle_timeout = config.Lookup<std::size_t>("scheduler.idle_worker_timeout_ms", target->scheduler.idle_worker_timeout.count(), "空闲 M 回收毫秒");
    auto sysmon_interval = config.Lookup<std::size_t>("scheduler.sysmon_interval_ms", target->scheduler.sysmon_interval.count(), "sysmon 周期毫秒");
    auto syscall_threshold = config.Lookup<std::size_t>("scheduler.long_syscall_threshold_ms", target->scheduler.long_syscall_threshold.count(), "长系统调用阈值毫秒");
    if (!idle_wait || !idle_timeout || !sysmon_interval || !syscall_threshold) valid = false;
    if (idle_wait) {
        idle_wait->SetValidator([](const std::size_t& value, std::string* validation_error) {
            return ValidateDuration("idle_wait_ms", value, validation_error);
        });
        idle_wait->AddListener([target](const std::size_t&, const std::size_t& value) {
            target->scheduler.idle_wait = std::chrono::milliseconds(value);
        });
    }
    if (idle_timeout) {
        idle_timeout->SetValidator([](const std::size_t& value, std::string* validation_error) {
            return ValidateDuration("idle_worker_timeout_ms", value, validation_error);
        });
        idle_timeout->AddListener([target](const std::size_t&, const std::size_t& value) {
            target->scheduler.idle_worker_timeout = std::chrono::milliseconds(value);
        });
    }
    if (sysmon_interval) {
        sysmon_interval->SetValidator([](const std::size_t& value, std::string* validation_error) {
            return ValidateDuration("sysmon_interval_ms", value, validation_error);
        });
        sysmon_interval->AddListener([target](const std::size_t&, const std::size_t& value) {
            target->scheduler.sysmon_interval = std::chrono::milliseconds(value);
        });
    }
    if (syscall_threshold) {
        syscall_threshold->SetValidator([](const std::size_t& value, std::string* validation_error) {
            return ValidateDuration("long_syscall_threshold_ms", value, validation_error);
        });
        syscall_threshold->AddListener([target](const std::size_t&, const std::size_t& value) {
            target->scheduler.long_syscall_threshold = std::chrono::milliseconds(value);
        });
    }
    // step7: collect_metrics 关闭可降低热路径开销，热更新允许即时切换
    auto collect_metrics = config.Lookup<bool>(
        "scheduler.collect_metrics", target->scheduler.collect_metrics,
        "是否采集调度计时指标；关闭可降低少量热路径开销");
    if (!collect_metrics) {
        valid = false;
    } else {
        collect_metrics->AddListener([target](const bool&, const bool& value) {
            target->scheduler.collect_metrics = value;
        });
    }
    // step8: timer 阈值存在跨字段约束，需要两类校验器配合
    auto timer_warn = config.Lookup<std::size_t>(
        "timer.warn_after_days", target->timer_warn_after_days,
        "定时器长时长 WARN 阈值，单位天，0 关闭");
    auto timer_error = config.Lookup<std::size_t>(
        "timer.error_after_days", target->timer_error_after_days,
        "定时器长时长 ERROR 阈值，单位天，0 关闭");
    if (!timer_warn || !timer_error) {
        valid = false;
    } else {
        const auto validate_timer = [](const std::size_t& value,
                                       std::string* validation_error) {
            if (value > 36500) {
                if (validation_error) *validation_error =
                    "timer 时长阈值不能超过 36500 天";
                return false;
            }
            return true;
        };
        timer_warn->SetValidator([validate_timer](const std::size_t& value,
                                                   std::string* validation_error) {
            return validate_timer(value, validation_error);
        });
        timer_error->SetValidator([validate_timer](const std::size_t& value,
                                                    std::string* validation_error) {
            return validate_timer(value, validation_error);
        });
        const std::weak_ptr<ConfigVar<std::size_t>> weak_timer_warn = timer_warn;
        const std::weak_ptr<ConfigVar<std::size_t>> weak_timer_error = timer_error;
        timer_warn->SetDirectValidator(
            [validate_timer, weak_timer_error](const std::size_t& value,
                                                std::string* validation_error) {
                if (!validate_timer(value, validation_error)) return false;
                const auto sibling = weak_timer_error.lock();
                if (!sibling) return true;
                const auto error_value = sibling->GetValue();
                if (value != 0 && error_value != 0 && value > error_value) {
                    if (validation_error) *validation_error =
                        "timer.warn_after_days 不能大于 timer.error_after_days";
                    return false;
                }
                return true;
            });
        timer_error->SetDirectValidator(
            [validate_timer, weak_timer_warn](const std::size_t& value,
                                               std::string* validation_error) {
                if (!validate_timer(value, validation_error)) return false;
                const auto sibling = weak_timer_warn.lock();
                if (!sibling) return true;
                const auto warn_value = sibling->GetValue();
                if (value != 0 && warn_value != 0 && value < warn_value) {
                    if (validation_error) *validation_error =
                        "timer.error_after_days 不能小于 timer.warn_after_days";
                    return false;
                }
                return true;
            });
        // warn/error 是一个跨字段约束。使用完整候选快照校验，允许用户在一份
        // 配置文件中把两个阈值同时迁移，而不会被字段应用顺序或旧值阻挡。
        config.AddTransactionValidator([](
            const std::map<std::string, std::string>& candidates,
            std::string* validation_error) {
            std::size_t warn = 0;
            std::size_t error = 0;
            const auto warn_it = candidates.find("timer.warn_after_days");
            const auto error_it = candidates.find("timer.error_after_days");
            if (warn_it == candidates.end() || error_it == candidates.end() ||
                !ValueCodec<std::size_t>::Decode(warn_it->second, &warn) ||
                !ValueCodec<std::size_t>::Decode(error_it->second, &error)) {
                if (validation_error) *validation_error = "timer 阈值候选值转换失败";
                return false;
            }
            if (warn != 0 && error != 0 && warn > error) {
                if (validation_error) *validation_error =
                    "timer.warn_after_days 不能大于 timer.error_after_days";
                return false;
            }
            return true;
        });
        // ConfigVar 持有自己的 Listener；回调若再强持有同一个 ConfigVar，
        // 就会形成 ConfigVar -> Listener -> ConfigVar 的引用环。弱引用既保留
        // 注册表拥有变量时的正常行为，也允许注册表析构时完整释放整组对象。
        const auto apply_timer_limits = [target, weak_timer_warn, weak_timer_error] {
            const auto warn_variable = weak_timer_warn.lock();
            const auto error_variable = weak_timer_error.lock();
            if (!warn_variable || !error_variable) return;
            const auto warn_value = warn_variable->GetValue();
            const auto error_value = error_variable->GetValue();
            target->timer_warn_after_days = warn_value;
            target->timer_error_after_days = error_value;
            auto limits = core::TimerService::Default().DurationLimits();
            limits.warn_after = std::chrono::hours(24 * warn_value);
            limits.error_after = std::chrono::hours(24 * error_value);
            (void)core::TimerService::Default().SetDurationLimits(limits);
        };
        timer_warn->AddListener(
            [apply_timer_limits](const std::size_t&, const std::size_t&) {
                apply_timer_limits();
            });
        timer_error->AddListener(
            [apply_timer_limits](const std::size_t&, const std::size_t&) {
                apply_timer_limits();
            });
    }
    // step9: 汇总类型冲突；已注册的监听器不回滚，调用方可重新绑定
    if (!valid && error) *error = "运行时配置变量类型冲突";
    return valid;
}


// ---------------------------------------------------------------------------
// 名字工具
// ---------------------------------------------------------------------------

std::pair<std::string, std::string> SplitConfigName(const std::string& full_name) {
    const auto dot = full_name.find('.');
    if (dot == std::string::npos) {
        return {std::string{}, full_name};
    }
    return {full_name.substr(0, dot), full_name.substr(dot + 1)};
}

std::string JoinConfigName(const std::string& module, const std::string& key) {
    if (module.empty()) {
        return key;
    }
    if (key.empty()) {
        return module;
    }
    return module + "." + key;
}

namespace {

/// 把值里会破坏 INI 结构或首尾空白的字节编码成 `\xHH`。
/// @details 编码范围：反斜杠、`#`、`;`、换行/回车/制表/NUL，以及值首尾的
///          空格（内部空格不动，保持可读）。这是与 `ExtractIniValue` 成对的
///          双向约定：DumpIni 的输出读回后与原值逐字节相等。
///          `#`/`;` 必须编码，否则读回时会被当成行内注释截断；首尾空格必须
///          编码，否则会被 Parse 的 Trim 吃掉。
std::string EscapeIniValue(const std::string& input) {
    static const char kHexDigits[] = "0123456789abcdef";
    const auto first_not_space = input.find_first_not_of(' ');
    const auto last_not_space = input.find_last_not_of(' ');
    std::string out;
    out.reserve(input.size() + 8);
    for (std::size_t position = 0; position < input.size(); ++position) {
        const char character = input[position];
        const bool edge_space =
            character == ' ' &&
            (first_not_space == std::string::npos || position < first_not_space ||
             position > last_not_space);
        const bool must_escape =
            edge_space || character == '\\' || character == '#' ||
            character == ';' || character == '\n' || character == '\r' ||
            character == '\t' || character == '\0';
        if (!must_escape) {
            out += character;
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        out += '\\';
        out += 'x';
        out += kHexDigits[byte >> 4U];
        out += kHexDigits[byte & 0x0FU];
    }
    return out;
}

/// 描述文本按行前缀 `# `，空描述返回空字符串。
std::string RenderComment(const std::string& description) {
    if (description.empty()) {
        return {};
    }
    std::string out;
    std::size_t start = 0;
    while (start <= description.size()) {
        const auto end = description.find('\n', start);
        const auto line = description.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty()) {
            out += "# " + line + "\n";
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

}  // namespace

bool Config::Remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_vars.erase(name) > 0;
}

std::map<std::string, std::vector<ConfigVarBase::ptr>> Config::Groups() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, std::vector<ConfigVarBase::ptr>> groups;
    for (const auto& [name, var] : m_vars) {
        const auto [module, key] = SplitConfigName(name);
        (void)key;
        groups[module].push_back(var);
    }
    // 组内按完整名字典序排列，保证 DumpIni 输出稳定可 diff。
    for (auto& [module, vars] : groups) {
        (void)module;
        std::sort(vars.begin(), vars.end(),
                  [](const ConfigVarBase::ptr& a, const ConfigVarBase::ptr& b) {
                      if (!a) return false;
                      if (!b) return true;
                      return a->name() < b->name();
                  });
    }
    return groups;
}

std::string Config::DumpIni() const {
    const auto groups = Groups();
    std::string out;
    out += "# go2cpp 配置（由 Config::DumpIni 生成）\n";
    out += "# 每个 [section] 对应一个 module，条目形如 key=value；# 开头为注释。\n";
    out += "# 值中的 \\xHH 表示原始字节（如 \\x0a 换行、\\x23 #、\\x3b ;），"
           "IniFile::Parse 会还原。\n";

    for (const auto& [module, vars] : groups) {
        out += "\n";
        out += "[" + (module.empty() ? std::string("global") : module) + "]\n";
        for (const auto& var : vars) {
            if (!var) {
                continue;
            }
            const auto [var_module, key] = SplitConfigName(var->name());
            (void)var_module;
            out += RenderComment(var->description());
            out += key + "=" + EscapeIniValue(var->ToString()) + "\n";
        }
    }
    return out;
}

bool Config::SaveToFile(const std::string& path, std::string* error) const {
    if (path.empty()) {
        if (error) {
            *error = "配置导出路径为空";
        }
        return false;
    }
    const std::string text = DumpIni();
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "无法打开临时文件: " + tmp_path;
            }
            return false;
        }
        out << text;
        out.flush();
        if (!out) {
            if (error) {
                *error = "写入临时文件失败: " + tmp_path;
            }
            (void)std::remove(tmp_path.c_str());
            return false;
        }
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        if (error) {
            *error = "重命名失败: " + tmp_path + " -> " + path;
        }
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

}  // namespace go2cpp::config