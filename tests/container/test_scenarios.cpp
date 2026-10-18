/**
 * @file test_scenarios.cpp
 * @brief 真实场景测试（6 个）：覆盖 Heap / List / Ring 的全部待验证能力。
 *
 * ## 场景
 * 1. 优先队列调度：按 deadline 排序任务，验证出队顺序与集合完整性；
 * 2. LRU 缓存：`List` + `unordered_map`，验证访问顺序、容量淘汰与指针稳定；
 * 3. 生产者-消费者队列：`List` 做 FIFO，多生产者/多消费者 + 单生产者严格顺序；
 * 4. 轮询调度器：`Ring` 循环分配任务，验证轮转顺序与负载均衡；
 * 5. 合并 K 个有序数组：`Heap` 多路归并，验证结果有序且元素守恒；
 * 6. Dijkstra 最短路：`Heap<pair>` 做优先队列，与 O(V²) 参考实现对比距离。
 */
#include "container_test_support.hpp"

#include "go2cpp/container/container.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace container = go2cpp::container;

namespace {

// ===========================================================================
// 场景 1：优先队列调度（按 deadline）
// ===========================================================================
struct ScheduledTask {
    int deadline;
    int id;
};

struct DeadlineLess {
    bool operator()(const ScheduledTask& left, const ScheduledTask& right) const {
        if (left.deadline != right.deadline) {
            return left.deadline < right.deadline;
        }
        return left.id < right.id;
    }
};

/** @brief Go 接口式的任务堆：只负责追加/删除末尾，sift 由自由函数完成。 */
class TaskInterfaceHeap {
 public:
    std::size_t Len() const { return m_data.size(); }

    bool Less(std::size_t i, std::size_t j) const {
        return DeadlineLess{}(m_data[i], m_data[j]);
    }

    void Swap(std::size_t i, std::size_t j) {
        std::swap(m_data[i], m_data[j]);
    }

    void Push(ScheduledTask task) { m_data.push_back(task); }

    ScheduledTask Pop() {
        const ScheduledTask task = m_data.back();
        m_data.pop_back();
        return task;
    }

 private:
    std::vector<ScheduledTask> m_data;
};

void ScenarioPriorityQueueScheduler() {
    go2cpp_container_test::Announce(
        "scenario 1: deadline priority-queue scheduler");

    constexpr int kTasks = 20000;
    std::mt19937 rng(2026U);
    std::vector<ScheduledTask> tasks;
    tasks.reserve(kTasks);
    for (int i = 0; i < kTasks; ++i) {
        tasks.push_back(ScheduledTask{static_cast<int>(rng() % 10000U), i});
    }

    container::Heap<ScheduledTask, DeadlineLess> template_heap;
    TaskInterfaceHeap interface_heap;
    for (const ScheduledTask& task : tasks) {
        template_heap.Push(task);
        interface_heap.Push(task);
    }
    container::Init(interface_heap);

    // 运行中插入一个紧急任务（deadline 最小），两种形态都必须最先弹出。
    template_heap.Push(ScheduledTask{-1, kTasks});
    container::Push(interface_heap, ScheduledTask{-1, kTasks});

    std::vector<int> template_order;
    std::vector<int> interface_order;
    int last_deadline = -100;
    int last_id = -1;
    while (!template_heap.Empty()) {
        const ScheduledTask task = template_heap.Pop();
        CONTAINER_CHECK(task.deadline >= last_deadline);
        if (task.deadline == last_deadline) {
            CONTAINER_CHECK(task.id > last_id);
        }
        last_deadline = task.deadline;
        last_id = task.id;
        template_order.push_back(task.id);
    }

    last_deadline = -100;
    last_id = -1;
    while (interface_heap.Len() > 0) {
        const ScheduledTask task = container::Pop(interface_heap);
        CONTAINER_CHECK(task.deadline >= last_deadline);
        if (task.deadline == last_deadline) {
            CONTAINER_CHECK(task.id > last_id);
        }
        last_deadline = task.deadline;
        last_id = task.id;
        interface_order.push_back(task.id);
    }

    CONTAINER_CHECK(template_order.size() == static_cast<std::size_t>(kTasks + 1));
    CONTAINER_CHECK(interface_order == template_order);
    CONTAINER_CHECK(template_order.front() == kTasks);  // 紧急任务先出队

    std::vector<bool> seen(static_cast<std::size_t>(kTasks + 1), false);
    for (int id : template_order) {
        CONTAINER_REQUIRE(id >= 0 && id <= kTasks);
        CONTAINER_CHECK(!seen[static_cast<std::size_t>(id)]);
        seen[static_cast<std::size_t>(id)] = true;
    }
    for (bool value : seen) {
        CONTAINER_CHECK(value);
    }
}

// ===========================================================================
// 场景 2：LRU 缓存
// ===========================================================================
struct LruEntry {
    int key;
    int value;
};

/**
 * @brief 真实 LRU：`List` 保存访问顺序，`unordered_map` 保存稳定句柄。
 *
 * @details 把 `Element*` 放进 map 是本场景要验证的核心：只要 List 的指针稳定，
 *          其他 key 的访问/淘汰就不会让 map 里的句柄失效。
 */
class LruCache {
 public:
    explicit LruCache(std::size_t capacity) : m_capacity(capacity) {}

    bool Get(int key, int& out) {
        const auto it = m_index.find(key);
        if (it == m_index.end()) {
            return false;
        }
        out = it->second->Value.value;
        m_list.MoveToFront(it->second);
        return true;
    }

    void Put(int key, int value) {
        const auto it = m_index.find(key);
        if (it != m_index.end()) {
            it->second->Value.value = value;
            m_list.MoveToFront(it->second);
            return;
        }
        if (m_index.size() >= m_capacity) {
            const auto* back = m_list.Back();
            CONTAINER_REQUIRE(back != nullptr);
            const int evicted_key = back->Value.key;
            const auto erased = m_index.find(evicted_key);
            CONTAINER_REQUIRE(erased != m_index.end());
            m_index.erase(erased);
            m_list.Remove(m_list.Back());
        }
        auto* node = m_list.PushFront(LruEntry{key, value});
        m_index.emplace(key, node);
    }

    std::size_t Size() const { return m_index.size(); }

    /** @brief 校验「map 中的句柄仍指向链表中的同一节点」。 */
    bool Validate() {
        if (m_index.size() != m_list.Len()) {
            return false;
        }
        std::size_t count = 0;
        for (auto* node = m_list.Front(); node != nullptr; node = node->Next()) {
            const auto it = m_index.find(node->Value.key);
            if (it == m_index.end() || it->second != node) {
                return false;
            }
            ++count;
        }
        return count == m_index.size();
    }

    /** @brief 从最近使用到最久未使用的 (key, value) 快照。 */
    std::vector<std::pair<int, int>> Snapshot() {
        std::vector<std::pair<int, int>> out;
        out.reserve(m_index.size());
        for (auto* node = m_list.Front(); node != nullptr; node = node->Next()) {
            out.emplace_back(node->Value.key, node->Value.value);
        }
        return out;
    }

 private:
    std::size_t m_capacity;
    container::List<LruEntry> m_list;
    std::unordered_map<int, container::Element<LruEntry>*> m_index;
};

/** @brief 参考实现：std::list + 迭代器，语义与 LruCache 相同。 */
class ReferenceLruCache {
 public:
    explicit ReferenceLruCache(std::size_t capacity) : m_capacity(capacity) {}

    bool Get(int key, int& out) {
        const auto it = m_index.find(key);
        if (it == m_index.end()) {
            return false;
        }
        out = it->second->value;
        m_list.splice(m_list.begin(), m_list, it->second);
        return true;
    }

    void Put(int key, int value) {
        const auto it = m_index.find(key);
        if (it != m_index.end()) {
            it->second->value = value;
            m_list.splice(m_list.begin(), m_list, it->second);
            return;
        }
        if (m_index.size() >= m_capacity) {
            const int evicted_key = m_list.back().key;
            m_index.erase(evicted_key);
            m_list.pop_back();
        }
        m_list.push_front(LruEntry{key, value});
        m_index.emplace(key, m_list.begin());
    }

    std::vector<std::pair<int, int>> Snapshot() const {
        std::vector<std::pair<int, int>> out;
        out.reserve(m_list.size());
        for (const LruEntry& entry : m_list) {
            out.emplace_back(entry.key, entry.value);
        }
        return out;
    }

 private:
    std::size_t m_capacity;
    std::list<LruEntry> m_list;
    std::unordered_map<int, std::list<LruEntry>::iterator> m_index;
};

void ScenarioLruCache() {
    go2cpp_container_test::Announce("scenario 2: LRU cache (List + unordered_map)");

    constexpr std::size_t kCapacity = 128;
    constexpr int kOps = 200000;
    LruCache cache(kCapacity);
    ReferenceLruCache reference(kCapacity);

    std::mt19937 rng(12345U);
    std::size_t hits = 0;
    for (int op = 0; op < kOps; ++op) {
        const int key = static_cast<int>(rng() % 1000U);
        if (rng() % 100U < 55U) {
            int value = -1;
            int ref_value = -1;
            const bool hit = cache.Get(key, value);
            const bool ref_hit = reference.Get(key, ref_value);
            CONTAINER_CHECK(hit == ref_hit);
            if (hit) {
                CONTAINER_CHECK(value == ref_value);
                ++hits;
            }
        } else {
            const int value = key * 1000 + static_cast<int>(rng() % 1000U);
            cache.Put(key, value);
            reference.Put(key, value);
        }

        if (op % 100 == 0) {
            CONTAINER_CHECK(cache.Size() <= kCapacity);
            CONTAINER_CHECK(cache.Validate());
        }
        if (op % 1000 == 0) {
            CONTAINER_CHECK(cache.Snapshot() == reference.Snapshot());
        }
    }

    CONTAINER_CHECK(cache.Size() <= kCapacity);
    CONTAINER_CHECK(cache.Validate());
    CONTAINER_CHECK(cache.Snapshot() == reference.Snapshot());

    // 访问一个已存在 key 必须把它移到最前；访问不存在的 key 不得改变顺序。
    const auto before = cache.Snapshot();
    CONTAINER_REQUIRE(!before.empty());
    int ignored = 0;
    CONTAINER_CHECK(cache.Get(before.back().first, ignored));
    const auto after = cache.Snapshot();
    CONTAINER_REQUIRE(!after.empty());
    CONTAINER_CHECK(after.front().first == before.back().first);

    int miss_value = 0;
    CONTAINER_CHECK(!cache.Get(-12345, miss_value));
    CONTAINER_CHECK(cache.Validate());
    CONTAINER_CHECK(hits > 0);
}

// ===========================================================================
// 场景 3：生产者-消费者 FIFO 队列
// ===========================================================================
void ScenarioProducerConsumer() {
    go2cpp_container_test::Announce("scenario 3: producer/consumer FIFO queue");

    // ---- 3a. 多生产者 / 多消费者 ----
    {
        constexpr int kProducers = 2;
        constexpr int kConsumers = 3;
        constexpr int kPerProducer = 5000;

        container::List<int> queue;
        std::mutex mutex;
        std::condition_variable condition;
        bool finished = false;
        int last_seen[kProducers] = {-1, -1};
        int consumed[kProducers] = {0, 0};

        auto producer = [&](int producer_id) {
            const int base = producer_id * 100000;
            for (int i = 0; i < kPerProducer; ++i) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    queue.PushBack(base + i);
                }
                condition.notify_one();
            }
        };

        auto consumer = [&]() {
            while (true) {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [&]() { return queue.Len() != 0 || finished; });
                if (queue.Len() == 0) {
                    if (finished) {
                        return;
                    }
                    continue;
                }
                auto* node = queue.Front();
                const int value = node->Value;
                queue.Remove(node);

                // 在锁内按出队顺序校验：同一生产者的序号必须严格递增。
                const int producer_id = value / 100000;
                const int sequence = value % 100000;
                CONTAINER_REQUIRE(producer_id >= 0 && producer_id < kProducers);
                CONTAINER_CHECK(sequence > last_seen[producer_id]);
                last_seen[producer_id] = sequence;
                ++consumed[producer_id];
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(kProducers + kConsumers);
        for (int i = 0; i < kProducers; ++i) {
            threads.emplace_back(producer, i);
        }
        for (int i = 0; i < kConsumers; ++i) {
            threads.emplace_back(consumer);
        }

        for (int i = 0; i < kProducers; ++i) {
            threads[static_cast<std::size_t>(i)].join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
        }
        condition.notify_all();
        for (int i = 0; i < kConsumers; ++i) {
            threads[static_cast<std::size_t>(kProducers + i)].join();
        }

        CONTAINER_CHECK(queue.Len() == 0);
        for (int i = 0; i < kProducers; ++i) {
            CONTAINER_CHECK(consumed[i] == kPerProducer);
            CONTAINER_CHECK(last_seen[i] == kPerProducer - 1);
        }
    }

    // ---- 3b. 单生产者 / 单消费者：严格验证全局 FIFO ----
    {
        constexpr int kItems = 50000;
        container::List<int> queue;
        std::mutex mutex;
        std::condition_variable condition;
        bool finished = false;
        std::vector<int> received;
        received.reserve(kItems);

        std::thread producer([&]() {
            for (int i = 0; i < kItems; ++i) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    queue.PushBack(i);
                }
                condition.notify_one();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                finished = true;
            }
            condition.notify_all();
        });

        std::thread consumer([&]() {
            while (true) {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [&]() { return queue.Len() != 0 || finished; });
                if (queue.Len() == 0) {
                    if (finished) {
                        return;
                    }
                    continue;
                }
                auto* node = queue.Front();
                const int value = node->Value;
                queue.Remove(node);
                lock.unlock();
                received.push_back(value);
            }
        });

        producer.join();
        consumer.join();

        CONTAINER_CHECK(queue.Len() == 0);
        CONTAINER_CHECK(received.size() == static_cast<std::size_t>(kItems));
        bool ordered = true;
        for (std::size_t i = 0; i < received.size(); ++i) {
            if (received[i] != static_cast<int>(i)) {
                ordered = false;
                break;
            }
        }
        CONTAINER_CHECK(ordered);
    }
}

// ===========================================================================
// 场景 4：轮询调度器
// ===========================================================================
void ScenarioRoundRobinScheduler() {
    go2cpp_container_test::Announce("scenario 4: round-robin Ring scheduler");

    constexpr int kWorkers = 8;
    constexpr int kRounds = 1000;
    container::Ring<int>* ring =
        container::Ring<int>::New(static_cast<std::size_t>(kWorkers));
    CONTAINER_REQUIRE(ring != nullptr);

    container::Ring<int>* cursor = ring;
    for (int i = 0; i < kWorkers; ++i) {
        cursor->Value = i;
        cursor = cursor->Next();
    }
    CONTAINER_CHECK(cursor == ring);

    // 逐 tick 分配 10000 个任务，worker 必须严格 0,1,...,7,0,1,...
    std::vector<int> worker_counts(kWorkers, 0);
    cursor = ring;
    for (int task = 0; task < kRounds * kWorkers; ++task) {
        const int worker = cursor->Value;
        CONTAINER_CHECK(worker == task % kWorkers);
        ++worker_counts[static_cast<std::size_t>(worker)];
        cursor = cursor->Next();
    }
    for (int count : worker_counts) {
        CONTAINER_CHECK(count == kRounds);
    }

    // 再按「轮次」模拟：每轮 8 个 worker 各处理一个任务，顺序必须是 0..7。
    cursor = ring;
    int completed = 0;
    for (int round = 0; round < kRounds; ++round) {
        for (int worker = 0; worker < kWorkers; ++worker) {
            CONTAINER_CHECK(cursor->Value == worker);
            cursor = cursor->Next();
            ++completed;
        }
    }
    CONTAINER_CHECK(completed == kRounds * kWorkers);
    CONTAINER_CHECK(cursor == ring);

    // Do 从任意起点都应完整遍历 8 个 worker。
    std::vector<int> order;
    ring->Move(3)->Do([&order](int& value) { order.push_back(value); });
    CONTAINER_CHECK(order == (std::vector<int>{3, 4, 5, 6, 7, 0, 1, 2}));

    container::Ring<int>::DeleteRing(ring);
}

// ===========================================================================
// 场景 5：合并 K 个有序数组
// ===========================================================================
struct MergeNode {
    int value;
    int array;
    int index;
};

struct MergeNodeLess {
    bool operator()(const MergeNode& left, const MergeNode& right) const {
        if (left.value != right.value) {
            return left.value < right.value;
        }
        if (left.array != right.array) {
            return left.array < right.array;
        }
        return left.index < right.index;
    }
};

void ScenarioMergeKSortedArrays() {
    go2cpp_container_test::Announce("scenario 5: merge K sorted arrays via Heap");

    constexpr int kArrays = 16;
    constexpr int kPerArray = 5000;
    std::mt19937 rng(8080U);

    std::vector<std::vector<int>> arrays;
    arrays.reserve(kArrays);
    std::vector<int> reference;
    reference.reserve(kArrays * kPerArray);
    for (int a = 0; a < kArrays; ++a) {
        std::vector<int> values;
        values.reserve(kPerArray);
        for (int i = 0; i < kPerArray; ++i) {
            values.push_back(static_cast<int>(rng() % 1000000U));
        }
        std::sort(values.begin(), values.end());
        reference.insert(reference.end(), values.begin(), values.end());
        arrays.push_back(std::move(values));
    }
    std::sort(reference.begin(), reference.end());

    container::Heap<MergeNode, MergeNodeLess> heap;
    for (int a = 0; a < kArrays; ++a) {
        heap.Push(MergeNode{arrays[static_cast<std::size_t>(a)][0], a, 0});
    }

    std::vector<int> merged;
    merged.reserve(reference.size());
    while (!heap.Empty()) {
        const MergeNode node = heap.Pop();
        merged.push_back(node.value);
        const int next_index = node.index + 1;
        if (next_index < kPerArray) {
            heap.Push(MergeNode{
                arrays[static_cast<std::size_t>(node.array)]
                      [static_cast<std::size_t>(next_index)],
                node.array, next_index});
        }
    }

    CONTAINER_CHECK(merged.size() == reference.size());
    CONTAINER_CHECK(merged == reference);
    CONTAINER_CHECK(std::is_sorted(merged.begin(), merged.end()));
    CONTAINER_CHECK(heap.Empty());
}

// ===========================================================================
// 场景 6：Dijkstra 最短路
// ===========================================================================
void ScenarioDijkstra() {
    go2cpp_container_test::Announce("scenario 6: Dijkstra shortest paths via Heap");

    constexpr int kNodes = 2000;
    constexpr int kExtraEdges = 8000;
    constexpr long long kInfinity = std::numeric_limits<long long>::max() / 4;

    std::mt19937 rng(4321U);
    std::vector<std::vector<std::pair<int, int>>> adjacency(
        static_cast<std::size_t>(kNodes));

    auto add_edge = [&](int from, int to, int weight) {
        adjacency[static_cast<std::size_t>(from)].emplace_back(to, weight);
    };

    // 先连成一棵树，保证从 0 可达所有节点。
    for (int node = 1; node < kNodes; ++node) {
        const int parent = static_cast<int>(rng() % static_cast<std::uint32_t>(node));
        const int weight = 1 + static_cast<int>(rng() % 100U);
        add_edge(parent, node, weight);
        add_edge(node, parent, weight);
    }
    for (int i = 0; i < kExtraEdges; ++i) {
        const int from = static_cast<int>(rng() % kNodes);
        const int to = static_cast<int>(rng() % kNodes);
        if (from == to) {
            continue;
        }
        const int weight = 1 + static_cast<int>(rng() % 100U);
        add_edge(from, to, weight);
    }

    // 被测实现：堆里放 (distance, node)，用重复入堆替代 decrease-key。
    std::vector<long long> distances(static_cast<std::size_t>(kNodes), kInfinity);
    distances[0] = 0;
    container::Heap<std::pair<long long, int>, std::greater<>> heap;
    heap.Push(std::make_pair(0LL, 0));
    while (!heap.Empty()) {
        const auto current = heap.Pop();
        const long long distance = current.first;
        const int node = current.second;
        if (distance != distances[static_cast<std::size_t>(node)]) {
            continue;  // 过期条目
        }
        for (const auto& edge : adjacency[static_cast<std::size_t>(node)]) {
            const long long candidate = distance + edge.second;
            if (candidate < distances[static_cast<std::size_t>(edge.first)]) {
                distances[static_cast<std::size_t>(edge.first)] = candidate;
                heap.Push(std::make_pair(candidate, edge.first));
            }
        }
    }

    // 参考实现：O(V²) 朴素 Dijkstra，不依赖任何堆。
    std::vector<long long> reference(static_cast<std::size_t>(kNodes), kInfinity);
    std::vector<bool> used(static_cast<std::size_t>(kNodes), false);
    reference[0] = 0;
    for (int iteration = 0; iteration < kNodes; ++iteration) {
        int best = -1;
        for (int node = 0; node < kNodes; ++node) {
            if (used[static_cast<std::size_t>(node)]) {
                continue;
            }
            if (best == -1 ||
                reference[static_cast<std::size_t>(node)] <
                    reference[static_cast<std::size_t>(best)]) {
                best = node;
            }
        }
        if (best == -1 || reference[static_cast<std::size_t>(best)] == kInfinity) {
            break;
        }
        used[static_cast<std::size_t>(best)] = true;
        for (const auto& edge : adjacency[static_cast<std::size_t>(best)]) {
            const long long candidate =
                reference[static_cast<std::size_t>(best)] + edge.second;
            if (candidate < reference[static_cast<std::size_t>(edge.first)]) {
                reference[static_cast<std::size_t>(edge.first)] = candidate;
            }
        }
    }

    CONTAINER_CHECK(distances == reference);
    CONTAINER_CHECK(distances[0] == 0);

    // 逐节点的具体值也抽查：所有距离必须有限（图连通），且为正。
    std::size_t reachable = 0;
    for (long long distance : distances) {
        if (distance < kInfinity) {
            ++reachable;
            CONTAINER_CHECK(distance >= 0);
        }
    }
    CONTAINER_CHECK(reachable == static_cast<std::size_t>(kNodes));
}

}  // namespace

void RunScenarioTests() {
    ScenarioPriorityQueueScheduler();
    ScenarioLruCache();
    ScenarioProducerConsumer();
    ScenarioRoundRobinScheduler();
    ScenarioMergeKSortedArrays();
    ScenarioDijkstra();
}
