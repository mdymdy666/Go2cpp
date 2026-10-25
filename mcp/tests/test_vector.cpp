/**
 * @file test_vector.cpp
 * @brief 向量层单元测试：编码确定性、余弦、索引 top-k、持久化、LRU 缓存。
 */

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "mcp/vector/embedding.hpp"
#include "mcp/vector/vector_index.hpp"
#include "test_util.hpp"

namespace {

bool NearlyEqual(float left, float right, float epsilon = 1e-4F) {
    return std::fabs(left - right) <= epsilon;
}

}  // namespace

int main() {
    using namespace go2cpp::mcp;
    using namespace go2cpp::mcp::test;

    vector::HashEmbeddingProvider provider(256);
    std::string error;

    // 1. 相同文本 → 余弦 1；不同主题 → 明显更低。
    vector::Embedding sqrt_a;
    vector::Embedding sqrt_b;
    vector::Embedding weather;
    CHECK(provider.Embed("计算平方根", sqrt_a, error));
    CHECK(provider.Embed("计算平方根", sqrt_b, error));
    CHECK(provider.Embed("查询今天天气", weather, error));
    CHECK_EQ(std::to_string(sqrt_a.size()), "256");
    CHECK(NearlyEqual(vector::CosineSimilarity(sqrt_a, sqrt_b), 1.0F));
    CHECK(vector::CosineSimilarity(sqrt_a, weather) < 0.8F);

    // 2. 确定性：同一文本两次编码逐元素相等。
    bool identical = true;
    for (std::size_t index = 0; index < sqrt_a.size(); ++index) {
        if (sqrt_a[index] != sqrt_b[index]) {
            identical = false;
            break;
        }
    }
    CHECK(identical);

    // 3. 空文本 → 零向量，范数为 0，不抛异常。
    vector::Embedding empty;
    CHECK(provider.Embed("", empty, error));
    CHECK(vector::Norm(empty) == 0.0F);
    CHECK(vector::CosineSimilarity(empty, sqrt_a) == 0.0F);

    // 4. 索引 top-k 与排序。
    vector::VectorIndex index;
    index.Add("sqrt", sqrt_a);
    index.Add("weather", weather);
    CHECK_EQ(std::to_string(index.size()), "2");
    {
        const auto hits = index.Search(sqrt_a, 1);
        CHECK_EQ(std::to_string(hits.size()), "1");
        CHECK_EQ(hits.empty() ? "" : hits[0].id, "sqrt");
    }
    {
        const auto hits = index.Search(sqrt_a, 5, 0.99F);
        CHECK_EQ(std::to_string(hits.size()), "1");  // min_score 过滤掉 weather
    }

    // 5. 维度不一致的条目被跳过（不崩溃、不误命中）。
    {
        vector::VectorIndex mixed;
        mixed.Add("short", vector::Embedding{1.0F, 0.0F});
        mixed.Add("long", sqrt_a);
        const auto hits = mixed.Search(sqrt_a, 10);
        CHECK_EQ(std::to_string(hits.size()), "1");
        CHECK_EQ(hits.empty() ? "" : hits[0].id, "long");
    }

    // 6. 持久化往返。
    {
        const std::string path = "/tmp/mcp_test_vector_index.txt";
        CHECK(index.Save(path, &error));
        vector::VectorIndex restored;
        CHECK(restored.Load(path, &error));
        CHECK_EQ(std::to_string(restored.size()), "2");
        CHECK(restored.Contains("sqrt"));
        CHECK(restored.Contains("weather"));
        CHECK(!restored.Remove("missing"));
        CHECK(restored.Remove("weather"));
        CHECK_EQ(std::to_string(restored.size()), "1");
        std::remove(path.c_str());
    }

    // 6.5 按 id 取向量副本（文档增量更新复用未变化块的基础）。
    {
        vector::Embedding copy;
        CHECK(index.Get("sqrt", copy));
        CHECK_EQ(std::to_string(copy.size()), "256");
        copy[0] += 1.0F;  // 改副本不影响索引里的向量
        vector::Embedding original;
        CHECK(index.Get("sqrt", original));
        CHECK(original[0] != copy[0]);
        CHECK(!index.Get("missing", copy));
    }

    // 7. LRU 缓存：命中/未命中计数与容量淘汰。
    {
        vector::EmbeddingCache cache(2);
        vector::Embedding value;
        CHECK(!cache.Get("a", value));
        cache.Put("a", sqrt_a);
        cache.Put("b", weather);
        CHECK(cache.Get("a", value));
        CHECK(cache.Get("b", value));
        cache.Put("c", empty);  // 淘汰最久未使用的 a（b 刚被访问）
        CHECK(!cache.Get("a", value));
        CHECK(cache.Get("b", value));
        CHECK(cache.Get("c", value));
        const auto stats = cache.Snapshot();
        CHECK_EQ(std::to_string(stats.entries), "2");
        CHECK(stats.hits >= 4);
        CHECK(stats.misses >= 1);
    }

    // 8. 余弦边界：零向量、空向量、维度不一致。
    {
        const vector::Embedding zeros(3, 0.0F);
        const vector::Embedding ones(3, 1.0F);
        CHECK(vector::CosineSimilarity(zeros, ones) == 0.0F);
        CHECK(vector::CosineSimilarity(vector::Embedding{}, ones) == 0.0F);
        CHECK(vector::CosineSimilarity(ones, vector::Embedding{1.0F}) == 0.0F);
    }

    return Summary("vector");
}
