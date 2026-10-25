/**
 * @file test_retrieval.cpp
 * @brief 检索层单元测试：工具检索、增量注册/覆盖/注销、增量与全量一致性、
 *        文档增量更新、并发注册与检索、应用层自动增量装配。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mcp/app/application.hpp"
#include "mcp/retrieval/document_store.hpp"
#include "mcp/retrieval/tool_retriever.hpp"
#include "mcp/server/registry.hpp"
#include "mcp/vector/embedding.hpp"
#include "test_util.hpp"

namespace {

go2cpp::mcp::Tool MakeTool(const std::string& name, const std::string& description) {
    go2cpp::mcp::Tool tool;
    tool.name = name;
    tool.description = description;
    tool.input_schema = go2cpp::mcp::json{{"type", "object"}};
    tool.handler = [](const go2cpp::mcp::ToolContext&) { return go2cpp::mcp::ToolResult::Text("ok"); };
    return tool;
}

/// 记录 Embed 调用次数的装饰器：用来证明增量更新只向量化变化的条目，
/// 而不是每次注册表变化都全量重建索引。
class CountingProvider final : public go2cpp::mcp::vector::IEmbeddingProvider {
public:
    explicit CountingProvider(std::size_t dimension) : m_inner(dimension) {}

    std::string Name() const override { return m_inner.Name(); }
    std::size_t Dimension() const override { return m_inner.Dimension(); }

    bool Embed(std::string_view text, go2cpp::mcp::vector::Embedding& out, std::string& error) override {
        m_calls.fetch_add(1);
        return m_inner.Embed(text, out, error);
    }

    std::size_t calls() const { return m_calls.load(); }
    void Reset() { m_calls.store(0); }

private:
    go2cpp::mcp::vector::HashEmbeddingProvider m_inner;
    std::atomic<std::size_t> m_calls{0};
};

/// 可切换失败的编码器：验证 embedding 失败时索引保持一致、可由 Refresh 兜底。
class SwitchableProvider final : public go2cpp::mcp::vector::IEmbeddingProvider {
public:
    explicit SwitchableProvider(std::size_t dimension) : m_inner(dimension) {}

    std::string Name() const override { return m_inner.Name(); }
    std::size_t Dimension() const override { return m_inner.Dimension(); }

    void SetFail(bool fail) { m_fail = fail; }

    bool Embed(std::string_view text, go2cpp::mcp::vector::Embedding& out, std::string& error) override {
        if (m_fail) {
            error = "测试注入的向量化失败";
            return false;
        }
        return m_inner.Embed(text, out, error);
    }

private:
    go2cpp::mcp::vector::HashEmbeddingProvider m_inner;
    bool m_fail = false;
};

/// 把检索结果规范化为「按 id 排序的 (id, score)」，消除容器迭代顺序差异。
std::vector<std::pair<std::string, float>> Canonical(
    const std::vector<go2cpp::mcp::retrieval::RetrievedItem>& items) {
    std::vector<std::pair<std::string, float>> canonical;
    canonical.reserve(items.size());
    for (const auto& item : items) {
        canonical.emplace_back(item.id, item.score);
    }
    std::sort(canonical.begin(), canonical.end(),
              [](const std::pair<std::string, float>& left,
                 const std::pair<std::string, float>& right) {
                  return left.first < right.first;
              });
    return canonical;
}

bool SameRanking(const std::vector<std::pair<std::string, float>>& left,
                 const std::vector<std::pair<std::string, float>>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].first != right[index].first) {
            return false;
        }
        if (std::fabs(left[index].second - right[index].second) > 1e-5F) {
            return false;
        }
    }
    return true;
}

bool SameHits(const std::vector<go2cpp::mcp::vector::SearchHit>& left,
              const std::vector<go2cpp::mcp::vector::SearchHit>& right) {
    std::vector<std::pair<std::string, float>> left_pairs;
    std::vector<std::pair<std::string, float>> right_pairs;
    for (const auto& hit : left) {
        left_pairs.emplace_back(hit.id, hit.score);
    }
    for (const auto& hit : right) {
        right_pairs.emplace_back(hit.id, hit.score);
    }
    std::sort(left_pairs.begin(), left_pairs.end());
    std::sort(right_pairs.begin(), right_pairs.end());
    return SameRanking(left_pairs, right_pairs);
}

bool ContainsId(const std::vector<go2cpp::mcp::retrieval::RetrievedItem>& items, const std::string& id) {
    return std::any_of(items.begin(), items.end(),
                       [&](const go2cpp::mcp::retrieval::RetrievedItem& item) { return item.id == id; });
}

}  // namespace

int main() {
    using namespace go2cpp::mcp;
    using namespace go2cpp::mcp::test;

    // 1. 未订阅（subscribe=false）保持懒构建：首次检索才建索引；Refresh 可兜底。
    {
        Registry registry;
        registry.RegisterTool(MakeTool("sqrt", "计算一个数的平方根"));
        registry.RegisterTool(MakeTool("weather", "查询城市天气与温度"));
        registry.RegisterTool(MakeTool("echo", "回显输入的文本内容"));

        CountingProvider provider(512);
        retrieval::ToolRetriever retriever(registry, provider, retrieval::RetrieveOptions{2, 0.05F},
                                           false);
        CHECK(!retriever.subscribed());
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "0");

        const auto hits = retriever.Retrieve("平方根");
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "3");
        CHECK(!hits.empty());
        CHECK_EQ(hits.empty() ? "" : hits[0].id, "sqrt");
        CHECK_EQ(retriever.LastError(), "ok");

        const auto one = retriever.Retrieve("天气", 1);
        CHECK_EQ(std::to_string(one.size()), "1");
        CHECK_EQ(one.empty() ? "" : one[0].id, "weather");

        const json context = retriever.ContextFor("平方根");
        CHECK(context.is_array());
        CHECK(!context.empty());
        CHECK(context[0].contains("name"));
        CHECK(context[0].contains("inputSchema"));
        CHECK(context[0].contains("score"));

        CHECK(retriever.Refresh());
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "3");
    }

    // 2. 增量注册：不调用 Refresh，索引立即可见，且只向量化新增的 1 条。
    {
        Registry registry;
        registry.RegisterTool(MakeTool("sqrt", "计算一个数的平方根"));
        CountingProvider provider(512);
        retrieval::ToolRetriever retriever(registry, provider);

        CHECK(retriever.subscribed());
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "1");
        CHECK_EQ(std::to_string(provider.calls()), "1");  // 构造时只对齐已有 1 条

        provider.Reset();
        registry.RegisterTool(MakeTool("mail", "发送电子邮件的工具"));
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "2");
        CHECK_EQ(std::to_string(provider.calls()), "1");  // 不是全量重建（否则 2 次）

        const auto hits = retriever.Retrieve("发送邮件", 1);
        CHECK(!hits.empty());
        CHECK_EQ(hits.empty() ? "" : hits[0].id, "mail");
        CHECK_EQ(std::to_string(provider.calls()), "2");  // 查询本身 1 次

        const auto stats = retriever.Stats();
        CHECK_EQ(std::to_string(stats.indexed), "2");
        CHECK_EQ(std::to_string(stats.embedded), "1");   // 只有新增的 mail
        CHECK_EQ(std::to_string(stats.rebuilds), "1");   // 只有构造时的首次对齐
        CHECK_EQ(std::to_string(stats.failures), "0");
    }

    // 3. 覆盖更新：文本不变复用向量；描述变化只重算这一条。
    {
        Registry registry;
        registry.RegisterTool(MakeTool("mail", "发送电子邮件的工具"));
        CountingProvider provider(512);
        retrieval::ToolRetriever retriever(registry, provider);
        provider.Reset();

        // 只换 handler（检索文本不变）：不调用 embedding。
        Tool same = MakeTool("mail", "发送电子邮件的工具");
        same.handler = [](const ToolContext&) { return ToolResult::Text("v2"); };
        registry.RegisterTool(std::move(same));
        CHECK_EQ(std::to_string(provider.calls()), "0");
        CHECK_EQ(std::to_string(retriever.Stats().reused), "1");
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "1");

        // 描述变化：只重算这一条。
        registry.RegisterTool(MakeTool("mail", "群发邮件并抄送收件人"));
        CHECK_EQ(std::to_string(provider.calls()), "1");
        const auto hits = retriever.Retrieve("抄送", 1);
        CHECK(!hits.empty());
        CHECK_EQ(hits.empty() ? "" : hits[0].id, "mail");

        const json context = retriever.ContextFor("抄送");
        CHECK(!context.empty());
        if (!context.empty()) {
            CHECK_EQ(context[0]["description"].get<std::string>(), "群发邮件并抄送收件人");
        }
    }

    // 4. 注销：注册表删除后索引条目立即消失，且不会出现在检索/ContextFor 中。
    {
        Registry registry;
        registry.RegisterTool(MakeTool("mail", "发送电子邮件的工具"));
        registry.RegisterTool(MakeTool("sqrt", "计算一个数的平方根"));
        CountingProvider provider(512);
        retrieval::ToolRetriever retriever(registry, provider);
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "2");

        CHECK(registry.UnregisterTool("mail"));
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "1");
        CHECK_EQ(std::to_string(retriever.Stats().removed), "1");
        CHECK_EQ(retriever.LastError(), "ok");

        const auto hits = retriever.Retrieve("发送邮件", 5);
        CHECK(!ContainsId(hits, "mail"));
        for (const auto& entry : retriever.ContextFor("发送邮件")) {
            CHECK(entry["name"].get<std::string>() != "mail");
        }

        CHECK(!registry.UnregisterTool("mail"));  // 重复注销
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "1");
    }

    // 5. 增量 vs 全量：同样最终状态，检索结果与条目集合一致。
    {
        Registry incremental;
        Registry full;
        CountingProvider provider(512);

        retrieval::ToolRetriever incremental_retriever(incremental, provider);
        incremental.RegisterTool(MakeTool("calc", "四则运算计算器"));
        incremental.RegisterTool(MakeTool("weather", "查询城市天气"));
        incremental.RegisterTool(MakeTool("mail", "发送电子邮件"));
        incremental.RegisterTool(MakeTool("weather", "查询城市天气与温度"));  // 覆盖
        incremental.RegisterTool(MakeTool("temp", "临时工具"));
        CHECK(incremental.UnregisterTool("temp"));  // 注册后立即注销

        retrieval::ToolRetriever full_retriever(full, provider,
                                                retrieval::RetrieveOptions{5, 0.05F}, false);
        full.RegisterTool(MakeTool("calc", "四则运算计算器"));
        full.RegisterTool(MakeTool("weather", "查询城市天气与温度"));
        full.RegisterTool(MakeTool("mail", "发送电子邮件"));
        CHECK(full_retriever.Refresh());  // 全量重建

        CHECK_EQ(std::to_string(incremental_retriever.IndexedTools()), "3");
        CHECK_EQ(std::to_string(full_retriever.IndexedTools()), "3");
        CHECK(incremental_retriever.IndexedIds() == full_retriever.IndexedIds());

        for (const char* query : {"计算", "天气温度", "邮件", "平方根"}) {
            const auto left = Canonical(incremental_retriever.Retrieve(query, 5));
            const auto right = Canonical(full_retriever.Retrieve(query, 5));
            CHECK(SameRanking(left, right));
        }
    }

    // 6. 并发注册/注销 + 检索：100 次/线程，不崩溃、不丢最终状态。
    {
        Registry registry;
        registry.RegisterTool(MakeTool("base", "基础工具"));
        CountingProvider provider(256);
        retrieval::ToolRetriever retriever(registry, provider);

        std::atomic<int> failures{0};
        std::atomic<int> searches{0};
        std::vector<std::thread> threads;

        for (int writer = 0; writer < 4; ++writer) {
            threads.emplace_back([&registry, writer, &failures] {
                try {
                    for (int round = 0; round < 100; ++round) {
                        const std::string name =
                            "w" + std::to_string(writer) + "_" + std::to_string(round);
                        registry.RegisterTool(MakeTool(name, "并发注册的临时工具"));
                        registry.UnregisterTool(name);
                    }
                } catch (...) {
                    failures.fetch_add(1);
                }
            });
        }
        for (int reader = 0; reader < 4; ++reader) {
            threads.emplace_back([&retriever, &searches, &failures] {
                try {
                    for (int round = 0; round < 100; ++round) {
                        const auto hits = retriever.Retrieve("并发注册的工具", 5);
                        (void)hits;
                        (void)retriever.ContextFor("基础工具");
                        (void)retriever.Stats();
                        searches.fetch_add(1);
                    }
                } catch (...) {
                    failures.fetch_add(1);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        CHECK_EQ(std::to_string(failures.load()), "0");
        CHECK_EQ(std::to_string(searches.load()), "400");
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "1");  // 只剩 base
        CHECK_EQ(retriever.LastError(), "ok");
        CHECK_EQ(std::to_string(retriever.Stats().failures), "0");
    }

    // 7. 文档增量：更新只重算变化的块，删除后不可检索。
    {
        CountingProvider provider(512);
        retrieval::DocumentStore store(provider, retrieval::ChunkOptions{64, 16});
        std::string error;
        const std::string version1 =
            "第一段：模型上下文协议定义了客户端与服务器的交互方式。\n"
            "第二段：向量检索把文本编码成向量后按余弦相似度排序。\n"
            "第三段：进程间通信可以使用管道共享内存或消息队列。\n"
            "第四段：参数校验层负责在调用工具前检查 JSON Schema。\n"
            "第五段：传输层支持标准输入输出与 HTTP SSE 两种模式。\n"
            "第六段：插件通过 C ABI 动态加载并在卸载时释放资源。\n";
        const std::string version2 =
            "第一段：模型上下文协议定义了客户端与服务器的交互方式。\n"
            "第二段：向量检索把文本编码成向量后按余弦相似度排序。\n"
            "第三段：进程间通信可以使用管道共享内存或消息队列。\n"
            "第四段：参数校验层负责在调用工具前检查 JSON Schema。\n"
            "第五段：传输层支持标准输入输出与 HTTP SSE 两种模式。\n"
            "第六段：插件增量注销后必须从检索索引中移除对应条目。\n";

        CHECK(store.AddDocument("doc:///demo.txt", version1, error));
        const std::size_t chunks_before = store.size();
        CHECK(chunks_before >= 4);
        CHECK_EQ(std::to_string(provider.calls()), std::to_string(chunks_before));

        // 更新：只有末尾变化，未变化的块复用向量。
        provider.Reset();
        CHECK(store.AddDocument("doc:///demo.txt", version2, error));
        CHECK(provider.calls() < chunks_before);  // 没有全量重算
        CHECK(provider.calls() >= 1);
        CHECK(store.Snapshot().reused > 0);
        CHECK_EQ(std::to_string(store.document_count()), "1");

        // 块内容验证：新内容已入库、旧内容被整体替换（不依赖检索排序）。
        bool saw_plugin = false;
        bool saw_stale = false;
        for (std::size_t index = 0; index < store.size() + 4; ++index) {
            const auto chunk = store.Get("doc:///demo.txt#" + std::to_string(index));
            if (!chunk) {
                continue;
            }
            if (chunk->text.find("插件增量注销") != std::string::npos) {
                saw_plugin = true;
            }
            if (chunk->text.find("卸载时释放资源") != std::string::npos) {
                saw_stale = true;
            }
        }
        CHECK(saw_plugin);
        CHECK(!saw_stale);
        CHECK(!store.Search("增量注销后必须从检索索引中移除", 2).empty());

        CHECK(store.RemoveDocument("doc:///demo.txt"));
        CHECK_EQ(std::to_string(store.size()), "0");
        CHECK_EQ(std::to_string(store.document_count()), "0");
        CHECK(!store.RemoveDocument("doc:///demo.txt"));
        CHECK(store.Search("向量检索", 3).empty());
    }

    // 8. 文档增量 vs 全量：块集合、文本、检索结果一致。
    {
        const std::string version1 =
            "第一段：模型上下文协议定义了客户端与服务器的交互方式。\n"
            "第二段：向量检索把文本编码成向量后按余弦相似度排序。\n"
            "第三段：进程间通信可以使用管道共享内存或消息队列。\n"
            "第四段：参数校验层负责在调用工具前检查 JSON Schema。\n";
        const std::string version2 =
            "第一段：模型上下文协议定义了客户端与服务器的交互方式。\n"
            "第二段：向量检索把文本编码成向量后按余弦相似度排序。\n"
            "第三段：进程间通信可以使用管道共享内存或消息队列。\n"
            "第四段：插件通过 C ABI 动态加载并在卸载时释放资源。\n";

        CountingProvider provider(512);
        std::string error;
        retrieval::DocumentStore incremental(provider, retrieval::ChunkOptions{64, 16});
        CHECK(incremental.AddDocument("doc:///same.txt", version1, error));
        CHECK(incremental.AddDocument("doc:///same.txt", version2, error));

        retrieval::DocumentStore full(provider, retrieval::ChunkOptions{64, 16});
        CHECK(full.AddDocument("doc:///same.txt", version2, error));

        CHECK_EQ(std::to_string(incremental.size()), std::to_string(full.size()));
        CHECK(SameHits(incremental.Search("插件动态加载", 5), full.Search("插件动态加载", 5)));
        CHECK(SameHits(incremental.Search("进程间通信", 5), full.Search("进程间通信", 5)));
        for (const auto& hit : incremental.Search("模型上下文", 5)) {
            const auto left = incremental.Get(hit.id);
            const auto right = full.Get(hit.id);
            CHECK(left.has_value() == right.has_value());
            if (left && right) {
                CHECK(left->text == right->text);
            }
        }
    }

    // 9. 手工增量接口（subscribe=false 时由调用方驱动）。
    {
        Registry registry;
        CountingProvider provider(512);
        retrieval::ToolRetriever retriever(registry, provider, retrieval::RetrieveOptions{5, 0.05F},
                                           false);
        CHECK(!retriever.subscribed());

        const Tool tool = MakeTool("manual", "手工加入索引的工具");
        CHECK(retriever.AddTool(tool));
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "1");
        CHECK(retriever.UpdateTool(tool));  // 文本未变 → 复用向量
        CHECK_EQ(std::to_string(retriever.Stats().reused), "1");
        CHECK(retriever.RemoveTool("manual"));
        CHECK(!retriever.RemoveTool("manual"));
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "0");
        CHECK(retriever.Retrieve("手工加入", 5).empty());
    }

    // 10. embedding 失败不破坏一致性：构造时对齐失败 → 注册恢复 → Refresh 兜底。
    {
        Registry registry;
        registry.RegisterTool(MakeTool("ok", "正常工具"));
        SwitchableProvider provider(64);
        provider.SetFail(true);

        retrieval::ToolRetriever retriever(registry, provider);
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "0");
        CHECK(retriever.LastError() != "ok");

        provider.SetFail(false);
        registry.RegisterTool(MakeTool("recovered", "恢复后的工具"));
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "1");
        CHECK_EQ(retriever.LastError(), "ok");

        CHECK(retriever.Refresh());
        CHECK_EQ(std::to_string(retriever.IndexedTools()), "2");
        CHECK(retriever.IndexedIds() == std::vector<std::string>({"ok", "recovered"}));
    }

    // 11. 应用层：装配后索引完整，运行时注册/注销自动增量（不调 Refresh）。
    {
        std::string error;
        auto application = app::Application::Create(app::AppOptions{}, &error);
        CHECK(application != nullptr);
        CHECK(error.empty());
        if (application) {
            const std::size_t registered = application->registry().tool_count();
            CHECK(registered >= 25);
            CHECK_EQ(std::to_string(application->retriever()->IndexedTools()),
                     std::to_string(registered));
            CHECK_EQ(std::to_string(application->retriever()->Stats().rebuilds), "1");

            application->RegisterTool(MakeTool("dynamic_tool", "运行时动态注册的演示工具"));
            CHECK_EQ(std::to_string(application->retriever()->IndexedTools()),
                     std::to_string(registered + 1));
            const auto hits = application->retriever()->Retrieve("动态注册", 1);
            CHECK(!hits.empty());
            CHECK_EQ(hits.empty() ? "" : hits[0].id, "dynamic_tool");

            // 验收点：直接调用真实的 search_tools 工具，确认运行时注册的工具
            // 立即可见（旧实现需要应用层手工 Refresh 才会出现）。
            ToolContext context;
            context.arguments = json{{"query", "动态注册"}, {"top_k", 5}};
            const auto search_after_register =
                application->registry().InvokeTool("search_tools", context);
            CHECK(search_after_register.has_value());
            if (search_after_register) {
                CHECK(search_after_register->content[0].text.find("dynamic_tool") !=
                      std::string::npos);
            }

            CHECK(application->UnregisterTool("dynamic_tool"));
            CHECK(!application->UnregisterTool("dynamic_tool"));
            CHECK_EQ(std::to_string(application->retriever()->IndexedTools()),
                     std::to_string(registered));
            CHECK(!ContainsId(application->retriever()->Retrieve("动态注册", 10), "dynamic_tool"));
            CHECK_EQ(std::to_string(application->retriever()->Stats().removed), "1");

            const auto search_after_removal =
                application->registry().InvokeTool("search_tools", context);
            CHECK(search_after_removal.has_value());
            if (search_after_removal) {
                CHECK(search_after_removal->content[0].text.find("dynamic_tool") ==
                      std::string::npos);
            }
        }
    }

    // 12. 随机顺序增删改（属性测试）：最终状态必须与全量重建一致。
    {
        std::mt19937 rng(20261024U);
        const std::vector<std::string> names = {
            "alpha", "beta",   "gamma", "delta",  "epsilon", "zeta", "eta", "theta",
            "iota",  "kappa",  "lambda", "mu",    "nu",      "xi",   "omicron", "pi",
        };
        const std::vector<std::string> descriptions = {
            "计算平方根与幂运算", "查询城市天气与温度", "发送电子邮件并抄送", "读取本地文件内容",
            "执行 shell 命令",    "查询数据库记录",     "生成图片缩略图",     "压缩归档目录",
        };

        Registry registry;
        CountingProvider provider(256);
        retrieval::ToolRetriever retriever(registry, provider);
        for (int step = 0; step < 200; ++step) {
            const std::string& name = names[rng() % names.size()];
            const std::string& description = descriptions[rng() % descriptions.size()];
            switch (rng() % 3) {
                case 0:
                case 1:
                    registry.RegisterTool(MakeTool(name, description));
                    break;
                default:
                    registry.UnregisterTool(name);
                    break;
            }
        }
        CHECK_EQ(std::to_string(retriever.IndexedTools()),
                 std::to_string(registry.tool_count()));

        // 用注册表最终状态做一次全量构建，逐项对比。
        Registry full;
        // 必须先把 ToolList() 存成局部变量：直接对临时 json 的 operator[] 结果
        // 做 range-for 会让临时对象在循环前析构（悬空引用）。
        const json listed = registry.ToolList();
        for (const auto& tool : listed["tools"]) {
            full.RegisterTool(MakeTool(tool.value("name", ""), tool.value("description", "")));
        }
        retrieval::ToolRetriever full_retriever(full, provider,
                                                 retrieval::RetrieveOptions{10, 0.05F}, false);
        CHECK(full_retriever.Refresh());
        CHECK(retriever.IndexedIds() == full_retriever.IndexedIds());
        CHECK_EQ(std::to_string(retriever.IndexedTools()),
                 std::to_string(full_retriever.IndexedTools()));

        for (int query = 0; query < 20; ++query) {
            const std::string& text = descriptions[rng() % descriptions.size()];
            const auto left = Canonical(retriever.Retrieve(text, 10));
            const auto right = Canonical(full_retriever.Retrieve(text, 10));
            CHECK(SameRanking(left, right));
        }
    }

    // 13. 压测：增量注册延迟 + 并发检索吞吐（数据供报告，不做绝对阈值断言）。
    {
        Registry registry;
        CountingProvider provider(512);
        constexpr int kInitialTools = 200;
        for (int index = 0; index < kInitialTools; ++index) {
            registry.RegisterTool(MakeTool("bench_" + std::to_string(index),
                                           "压测工具，负责第 " + std::to_string(index) + " 类任务"));
        }
        retrieval::ToolRetriever retriever(registry, provider, retrieval::RetrieveOptions{10, 0.05F});

        std::vector<double> register_latency;
        register_latency.reserve(200);
        for (int index = 0; index < 200; ++index) {
            const auto start = std::chrono::steady_clock::now();
            registry.RegisterTool(MakeTool("late_" + std::to_string(index),
                                           "后注册的工具，用于测量增量注册延迟"));
            const auto elapsed = std::chrono::steady_clock::now() - start;
            register_latency.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
        }
        std::sort(register_latency.begin(), register_latency.end());
        const auto percentile = [](const std::vector<double>& values, double ratio) {
            if (values.empty()) {
                return 0.0;
            }
            const std::size_t index = static_cast<std::size_t>(ratio * (values.size() - 1));
            return values[index];
        };

        constexpr int kThreads = 8;
        constexpr int kQueriesPerThread = 250;
        std::vector<std::vector<double>> per_thread(kThreads);
        std::vector<std::thread> workers;
        const auto start = std::chrono::steady_clock::now();
        for (int thread = 0; thread < kThreads; ++thread) {
            workers.emplace_back([&retriever, &per_thread, thread] {
                per_thread[thread].reserve(kQueriesPerThread);
                for (int round = 0; round < kQueriesPerThread; ++round) {
                    const std::string query = "计算第 " + std::to_string(round) + " 类任务";
                    const auto begin = std::chrono::steady_clock::now();
                    (void)retriever.Retrieve(query, 10);
                    const auto done = std::chrono::steady_clock::now();
                    per_thread[thread].push_back(
                        std::chrono::duration<double, std::micro>(done - begin).count());
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        const double total_us = std::chrono::duration<double, std::micro>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
        std::vector<double> all;
        for (const auto& values : per_thread) {
            all.insert(all.end(), values.begin(), values.end());
        }
        std::sort(all.begin(), all.end());
        std::printf(
            "[retrieval] 压测：索引 %zu 条；增量注册 200 次 P50=%.1fus P95=%.1fus P99=%.1fus；"
            "并发检索 %zu 次（%d 线程）吞吐=%.0f qps P50=%.1fus P95=%.1fus P99=%.1fus\n",
            retriever.IndexedTools(), percentile(register_latency, 0.50),
            percentile(register_latency, 0.95), percentile(register_latency, 0.99), all.size(),
            kThreads, static_cast<double>(all.size()) / (total_us / 1e6), percentile(all, 0.50),
            percentile(all, 0.95), percentile(all, 0.99));
        CHECK_EQ(std::to_string(retriever.IndexedTools()), std::to_string(kInitialTools + 200));
        CHECK_EQ(retriever.LastError(), "ok");
    }

    return Summary("retrieval");
}
