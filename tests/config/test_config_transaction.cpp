/**
 * @file test_config_transaction.cpp
 * @brief CONF-01 回归测试：并发 `LoadFromIni` 的「整批一次发布」原子性。
 *
 * ## 覆盖
 * 1. 确定性交错：T1 发布第一批期间，T2 加载另一份配置。监听器在发布阶段
 *    读到的两个字段必须来自同一批；修复前 T2 可以在 T1 的通知中途写入
 *    一个字段，让监听器读到 `a=2, b=1`（顺序 1,2）或 `a=1, b=2`（顺序 2,1）；
 * 2. 并发压测：两个线程反复加载互斥的两份快照、第三个线程反复加载非法
 *    快照；变量监听器与提交回调观察到的字段永远一致，所有合法加载成功、
 *    所有非法加载失败；
 * 3. 监听器内重入提交：事务锁可重入，同一线程再次 `LoadFromIni` 不会死锁。
 */
#include "go2cpp/config.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp;
using namespace go2cpp::config;
using namespace std::chrono_literals;

bool ParseIni(const char* text, IniFile* ini) {
    std::string error;
    return ini->Parse(text, &error);
}

/// 确定性交错：发布阶段不允许另一个事务插入未发布的半套值。
void ScenarioDeterministicInterleaving() {
    auto& cfg = Config::Instance();
    auto a = cfg.Lookup<int>("conf01.a", 0, "CONF-01 一致性字段 A");
    auto b = cfg.Lookup<int>("conf01.b", 0, "CONF-01 一致性字段 B");
    GO2CPP_REQUIRE(a != nullptr && b != nullptr);
    a->SetValue(0);
    b->SetValue(0);

    IniFile first;
    GO2CPP_REQUIRE(ParseIni("[conf01]\na=1\nb=1\n", &first));
    IniFile second;
    GO2CPP_REQUIRE(ParseIni("[conf01]\na=2\nb=2\n", &second));

    std::mutex mutex;
    std::condition_variable go_cv;
    std::condition_variable probe_cv;
    std::condition_variable observed_cv;
    bool go = false;
    bool probe_fired = false;
    bool observer_done = false;
    std::atomic<bool> orchestrating{true};
    std::atomic<bool> arm_probe{false};
    std::atomic<bool> probe_armed{false};
    std::atomic<bool> probe_used{false};
    std::atomic<int> inconsistent{0};
    int observed_a = -1;
    int observed_b = -1;

    const auto record_mixed = [&](const char* stage, int value_a, int value_b) {
        inconsistent.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[conf01] %s: 观察到混合配置 a=%d b=%d\n", stage,
                     value_a, value_b);
    };

    // T1 的监听器：发布到一半时唤醒 T2，随后读取两个字段。
    // 只有 a 上挂了监听器；b 的发布通知仍由 T1 在自己的通知循环里完成。
    const auto listener_id = a->AddListener([&](int, int) {
        if (!orchestrating.exchange(false)) {
            const int value_a = a->GetValue();
            const int value_b = b->GetValue();
            if (value_a != value_b) {
                record_mixed("后续发布", value_a, value_b);
            }
            return;
        }
        arm_probe.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            go = true;
        }
        go_cv.notify_all();
        // 修复后 T2 被事务锁挡住，这里会一直等到超时；旧实现里 T2 会先写入
        // 一个字段再停在第二个字段的 ApplyString 校验器里。
        {
            std::unique_lock<std::mutex> lock(mutex);
            (void)probe_cv.wait_for(lock, 500ms, [&] { return probe_fired; });
            observed_a = a->GetValue();
            observed_b = b->GetValue();
            if (observed_a != observed_b) {
                record_mixed("发布中途", observed_a, observed_b);
            }
            observer_done = true;
        }
        observed_cv.notify_all();
    });

    // ApplyString 阶段的探针：谁后写，谁的校验器就能看到「另一个字段已经
    // 是本次事务的新值 2」。ValidateString 也会调用 m_validator，所以探针
    // 只在事务校验器 arm 之后生效，避免在预检阶段误触发。
    const auto make_probe = [&](const ConfigVar<int>::ptr& other) {
        return [&, other](const int&, std::string*) {
            if (!probe_armed.load(std::memory_order_acquire)) {
                return true;
            }
            if (other->GetValue() != 2) {
                return true;
            }
            if (probe_used.exchange(true)) {
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                probe_fired = true;
            }
            probe_cv.notify_all();
            std::unique_lock<std::mutex> lock(mutex);
            (void)observed_cv.wait_for(lock, 500ms,
                                       [&] { return observer_done; });
            return true;
        };
    };
    a->SetValidator(make_probe(b));
    b->SetValidator(make_probe(a));

    // 事务校验器在全部 ValidateString 之后、所有 ApplyString 之前运行，
    // 正好用来给「ApplyString 探针」装弹。
    const auto transaction_validator_id = cfg.AddTransactionValidator(
        [&](const std::map<std::string, std::string>&, std::string*) {
            if (arm_probe.exchange(false)) {
                probe_armed.store(true, std::memory_order_release);
            }
            return true;
        });

    std::atomic<bool> second_ok{false};
    std::thread second_loader([&] {
        {
            std::unique_lock<std::mutex> lock(mutex);
            go_cv.wait(lock, [&] { return go; });
        }
        second_ok.store(cfg.LoadFromIni(second, nullptr),
                        std::memory_order_release);
    });
    const bool first_ok = cfg.LoadFromIni(first, nullptr);
    second_loader.join();

    a->DelListener(listener_id);
    a->SetValidator({});
    b->SetValidator({});
    cfg.DelTransactionValidator(transaction_validator_id);

    GO2CPP_CHECK(first_ok);
    GO2CPP_CHECK(inconsistent.load() == 0);
    GO2CPP_CHECK(second_ok.load());
    GO2CPP_CHECK(observed_a == observed_b);
    GO2CPP_CHECK(a->GetValue() == 2);
    GO2CPP_CHECK(b->GetValue() == 2);

    a->SetValue(0);
    b->SetValue(0);
}

/// 并发压测：观察者永远看到一致的一对字段，失败事务不可观察。
void ScenarioConcurrentLoads() {
    auto& cfg = Config::Instance();
    auto a = cfg.Lookup<int>("conf01.stress_a", 0, "CONF-01 压测字段 A");
    auto b = cfg.Lookup<int>("conf01.stress_b", 0, "CONF-01 压测字段 B");
    GO2CPP_REQUIRE(a != nullptr && b != nullptr);
    a->SetValue(0);
    b->SetValue(0);

    IniFile one;
    GO2CPP_REQUIRE(ParseIni("[conf01]\nstress_a=1\nstress_b=1\n", &one));
    IniFile two;
    GO2CPP_REQUIRE(ParseIni("[conf01]\nstress_a=2\nstress_b=2\n", &two));
    IniFile broken;
    GO2CPP_REQUIRE(
        ParseIni("[conf01]\nstress_a=3\nstress_b=not-a-number\n", &broken));

    std::atomic<int> inconsistent{0};
    std::atomic<int> observations{0};
    const auto observe = [&](const char* stage) {
        const int value_a = a->GetValue();
        const int value_b = b->GetValue();
        observations.fetch_add(1, std::memory_order_relaxed);
        if (value_a != value_b) {
            inconsistent.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[conf01] 压测 %s: 观察到混合配置 a=%d b=%d\n",
                         stage, value_a, value_b);
        }
    };
    const auto listener_a = a->AddListener([&](int, int) { observe("监听器 a"); });
    const auto listener_b = b->AddListener([&](int, int) { observe("监听器 b"); });
    const auto commit_id = cfg.AddCommitListener(
        [&](const std::vector<ConfigVarBase::ptr>&) { observe("提交回调"); });

    constexpr int kRounds = 300;
    std::atomic<int> good_ok{0};
    std::atomic<int> good_failed{0};
    std::atomic<int> bad_ok{0};
    std::atomic<int> bad_failed{0};
    const auto load = [&](const IniFile& ini, std::atomic<int>* ok,
                          std::atomic<int>* failed) {
        for (int i = 0; i < kRounds; ++i) {
            if (cfg.LoadFromIni(ini, nullptr)) {
                ok->fetch_add(1, std::memory_order_relaxed);
            } else {
                failed->fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    std::thread first([&] { load(one, &good_ok, &good_failed); });
    std::thread second([&] { load(two, &good_ok, &good_failed); });
    std::thread invalid([&] { load(broken, &bad_ok, &bad_failed); });
    first.join();
    second.join();
    invalid.join();

    a->DelListener(listener_a);
    b->DelListener(listener_b);
    cfg.DelCommitListener(commit_id);

    GO2CPP_CHECK(good_ok.load() == 2 * kRounds);
    GO2CPP_CHECK(good_failed.load() == 0);
    GO2CPP_CHECK(bad_ok.load() == 0);
    GO2CPP_CHECK(bad_failed.load() == kRounds);
    GO2CPP_CHECK(inconsistent.load() == 0);
    GO2CPP_CHECK(observations.load() > 0);

    a->SetValue(0);
    b->SetValue(0);
}

/// 监听器内重入提交：依赖 recursive_mutex，普通 mutex 会自死锁。
void ScenarioReentrantSubmit() {
    auto& cfg = Config::Instance();
    auto inner = cfg.Lookup<int>("conf01.reentrant_inner", 0, "CONF-01 重入字段");
    auto outer = cfg.Lookup<int>("conf01.reentrant_outer", 0, "CONF-01 重入触发字段");
    GO2CPP_REQUIRE(inner != nullptr && outer != nullptr);
    inner->SetValue(0);
    outer->SetValue(0);

    std::atomic<int> notifications{0};
    std::atomic<bool> nested_ok{false};
    const auto listener_id = outer->AddListener([&](int, int) {
        if (notifications.fetch_add(1, std::memory_order_relaxed) != 0) {
            return;
        }
        IniFile nested;
        if (ParseIni("[conf01]\nreentrant_inner=7\n", &nested)) {
            nested_ok.store(cfg.LoadFromIni(nested, nullptr),
                            std::memory_order_release);
        }
    });
    IniFile ini;
    GO2CPP_REQUIRE(ParseIni("[conf01]\nreentrant_outer=5\n", &ini));
    GO2CPP_CHECK(cfg.LoadFromIni(ini, nullptr));
    outer->DelListener(listener_id);

    GO2CPP_CHECK(nested_ok.load());
    GO2CPP_CHECK(inner->GetValue() == 7);
    outer->SetValue(0);
    inner->SetValue(0);
}

}  // namespace

void run_config_transaction_tests() {
    go2cpp_tests::announce("配置事务发布原子性（CONF-01）");
    ScenarioDeterministicInterleaving();
    ScenarioConcurrentLoads();
    ScenarioReentrantSubmit();
}
