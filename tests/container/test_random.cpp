/**
 * @file test_random.cpp
 * @brief 随机测试：随机操作序列 + 与标准库/手工模型的结果对比。
 *
 * ## 覆盖
 * 1. 模板式 Heap：20 万次随机 push/pop/top，与 `std::priority_queue`（min-heap）
 *    逐步对比；
 * 2. 接口式 Heap：随机 push / Remove / Fix 共 5000 次，与 `std::multiset` 对比，
 *    每一步都校验堆性质；
 * 3. List：10 万次随机 push/insert/remove/move，与 `std::list` 逐步对比；
 * 4. Ring：随机 `Unlink` 与「摘掉 next 起 k 个元素」的 oracle 对比；
 * 5. Ring：随机 `Link`/`Unlink` 往返，验证所有节点集合守恒、结构自洽。
 */
#include "container_test_support.hpp"

#include "go2cpp/container/container.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <queue>
#include <random>
#include <set>
#include <unordered_set>
#include <vector>

namespace container = go2cpp::container;

namespace {

using go2cpp_container_test::IntVectorHeap;
using IntRing = container::Ring<int>;

void VerifyMinHeap(const IntVectorHeap& heap) {
    const std::size_t n = heap.Len();
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t left = 2 * i + 1;
        const std::size_t right = left + 1;
        if (left < n) {
            CONTAINER_CHECK(!heap.Less(left, i));
        }
        if (right < n) {
            CONTAINER_CHECK(!heap.Less(right, i));
        }
    }
}

// ---------------------------------------------------------------------------
// 1. 模板式 Heap vs std::priority_queue
// ---------------------------------------------------------------------------
void TestTemplateHeapAgainstPriorityQueue() {
    go2cpp_container_test::Announce("random: Heap vs std::priority_queue");

    for (std::uint32_t seed = 1; seed <= 5; ++seed) {
        std::mt19937 rng(seed);
        container::Heap<int> ours;
        std::priority_queue<int, std::vector<int>, std::greater<int>> reference;

        for (int step = 0; step < 200000; ++step) {
            const int op = static_cast<int>(rng() % 100U);
            if (op < 55 || reference.empty()) {
                const int value = static_cast<int>(rng() % 100000U);
                ours.Push(value);
                reference.push(value);
            } else if (op < 90) {
                CONTAINER_REQUIRE(!ours.Empty());
                const int got = ours.Pop();
                const int expected = reference.top();
                reference.pop();
                CONTAINER_CHECK(got == expected);
            } else {
                CONTAINER_REQUIRE(!ours.Empty());
                CONTAINER_CHECK(ours.Top() == reference.top());
            }
            CONTAINER_CHECK(ours.Size() == reference.size());
        }

        while (!reference.empty()) {
            const int expected = reference.top();
            reference.pop();
            CONTAINER_CHECK(ours.Pop() == expected);
        }
        CONTAINER_CHECK(ours.Empty());
    }
}

// ---------------------------------------------------------------------------
// 2. 接口式 Heap：随机 Remove / Fix vs std::multiset
// ---------------------------------------------------------------------------
void TestInterfaceHeapRandomFixRemove() {
    go2cpp_container_test::Announce("random: interface Heap Remove/Fix vs multiset");

    std::mt19937 rng(20261018U);
    IntVectorHeap heap;
    std::multiset<int> model;

    for (int i = 0; i < 500; ++i) {
        const int value = static_cast<int>(rng() % 5000U);
        heap.Push(value);
        model.insert(value);
    }
    container::Init(heap);
    VerifyMinHeap(heap);

    for (int step = 0; step < 5000; ++step) {
        const int op = static_cast<int>(rng() % 3U);
        if (op == 0 || heap.Len() == 0) {
            const int value = static_cast<int>(rng() % 5000U);
            container::Push(heap, value);
            model.insert(value);
        } else if (op == 1) {
            const std::size_t index = static_cast<std::size_t>(rng() % heap.Len());
            const int expected = heap.Data()[index];
            const int got = container::Remove(heap, index);
            CONTAINER_CHECK(got == expected);
            const auto it = model.find(expected);
            CONTAINER_REQUIRE(it != model.end());
            model.erase(it);
        } else {
            const std::size_t index = static_cast<std::size_t>(rng() % heap.Len());
            const int old_value = heap.Data()[index];
            const int new_value = static_cast<int>(rng() % 5000U);
            heap.Data()[index] = new_value;
            container::Fix(heap, index);
            const auto it = model.find(old_value);
            CONTAINER_REQUIRE(it != model.end());
            model.erase(it);
            model.insert(new_value);
        }
        VerifyMinHeap(heap);
    }

    for (int expected : model) {
        CONTAINER_CHECK(container::Pop(heap) == expected);
        VerifyMinHeap(heap);
    }
    CONTAINER_CHECK(heap.Len() == 0);
}

// ---------------------------------------------------------------------------
// 3. List vs std::list
// ---------------------------------------------------------------------------
void CompareLists(container::List<int>& ours, const std::list<int>& reference) {
    CONTAINER_CHECK(ours.Len() == reference.size());
    const auto* node = ours.Front();
    for (int value : reference) {
        CONTAINER_REQUIRE(node != nullptr);
        CONTAINER_CHECK(node->Value == value);
        node = node->Next();
    }
    CONTAINER_CHECK(node == nullptr);
}

void TestListAgainstStdList() {
    go2cpp_container_test::Announce("random: List vs std::list");

    std::mt19937 rng(777U);
    container::List<int> ours;
    std::list<int> reference;
    std::vector<container::Element<int>*> our_nodes;
    std::vector<std::list<int>::iterator> ref_nodes;

    const auto erase_handle = [&](std::size_t index) {
        our_nodes[index] = our_nodes.back();
        our_nodes.pop_back();
        ref_nodes[index] = ref_nodes.back();
        ref_nodes.pop_back();
    };

    for (int step = 0; step < 100000; ++step) {
        const int op = static_cast<int>(rng() % 10U);
        const int value = static_cast<int>(rng() % 1000U);
        const bool empty = reference.empty();

        if (op == 0 || (empty && op < 5)) {
            auto* node = ours.PushFront(value);
            reference.push_front(value);
            our_nodes.push_back(node);
            ref_nodes.push_back(reference.begin());
        } else if (op == 1 || empty) {
            auto* node = ours.PushBack(value);
            reference.push_back(value);
            our_nodes.push_back(node);
            ref_nodes.push_back(std::prev(reference.end()));
        } else if (op == 2) {
            const std::size_t index =
                static_cast<std::size_t>(rng() % reference.size());
            auto* node = ours.InsertBefore(value, our_nodes[index]);
            CONTAINER_REQUIRE(node != nullptr);
            auto it = reference.insert(ref_nodes[index], value);
            our_nodes.push_back(node);
            ref_nodes.push_back(it);
        } else if (op == 3) {
            const std::size_t index =
                static_cast<std::size_t>(rng() % reference.size());
            auto* node = ours.InsertAfter(value, our_nodes[index]);
            CONTAINER_REQUIRE(node != nullptr);
            auto it = reference.insert(std::next(ref_nodes[index]), value);
            our_nodes.push_back(node);
            ref_nodes.push_back(it);
        } else if (op == 4) {
            const std::size_t index =
                static_cast<std::size_t>(rng() % (reference.size() + 1U));
            auto* node = ours.InsertValue(value, index);
            CONTAINER_REQUIRE(node != nullptr);
            auto it = reference.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(index));
            it = reference.insert(it, value);
            our_nodes.push_back(node);
            ref_nodes.push_back(it);
        } else if (op == 5) {
            const std::size_t index =
                static_cast<std::size_t>(rng() % reference.size());
            const int got = ours.Remove(our_nodes[index]);
            CONTAINER_CHECK(got == *ref_nodes[index]);
            reference.erase(ref_nodes[index]);
            erase_handle(index);
        } else if (op == 6) {
            const std::size_t index =
                static_cast<std::size_t>(rng() % reference.size());
            ours.MoveToFront(our_nodes[index]);
            reference.splice(reference.begin(), reference, ref_nodes[index]);
        } else if (op == 7) {
            const std::size_t index =
                static_cast<std::size_t>(rng() % reference.size());
            ours.MoveToBack(our_nodes[index]);
            reference.splice(reference.end(), reference, ref_nodes[index]);
        } else if (op == 8) {
            const std::size_t index =
                static_cast<std::size_t>(rng() % reference.size());
            const std::size_t mark =
                static_cast<std::size_t>(rng() % reference.size());
            if (index == mark) {
                continue;
            }
            ours.MoveBefore(our_nodes[index], our_nodes[mark]);
            reference.splice(ref_nodes[mark], reference, ref_nodes[index]);
        } else {
            const std::size_t index =
                static_cast<std::size_t>(rng() % reference.size());
            const std::size_t mark =
                static_cast<std::size_t>(rng() % reference.size());
            if (index == mark) {
                continue;
            }
            ours.MoveAfter(our_nodes[index], our_nodes[mark]);
            reference.splice(std::next(ref_nodes[mark]), reference,
                             ref_nodes[index]);
        }

        if (step % 1000 == 0) {
            CompareLists(ours, reference);
            if (!reference.empty()) {
                CONTAINER_CHECK(ours.Front()->Value == reference.front());
                CONTAINER_CHECK(ours.Back()->Value == reference.back());
            }
        }
    }

    CompareLists(ours, reference);
}

// ---------------------------------------------------------------------------
// 4. Ring 随机 Unlink vs 序列 oracle
// ---------------------------------------------------------------------------
IntRing* MakeValueRing(int n) {
    IntRing* ring = IntRing::New(static_cast<std::size_t>(n));
    if (ring == nullptr) {
        return nullptr;
    }
    IntRing* node = ring;
    for (int i = 0; i < n; ++i) {
        node->Value = i;
        node = node->Next();
    }
    return ring;
}

std::vector<int> RingValues(IntRing* ring) {
    std::vector<int> values;
    if (ring != nullptr) {
        ring->Do([&values](int& value) { values.push_back(value); });
    }
    return values;
}

void VerifyRingStructure(IntRing* ring, std::size_t expected_len) {
    CONTAINER_REQUIRE(ring != nullptr);
    CONTAINER_CHECK(ring->Len() == expected_len);
    IntRing* node = ring;
    for (std::size_t i = 0; i < expected_len; ++i) {
        CONTAINER_CHECK(node->Next()->Prev() == node);
        CONTAINER_CHECK(node->Prev()->Next() == node);
        node = node->Next();
    }
    CONTAINER_CHECK(node == ring);
}

void TestRingRandomUnlinkOracle() {
    go2cpp_container_test::Announce("random: Ring Unlink vs sequence oracle");

    std::mt19937 rng(99U);
    for (int trial = 0; trial < 20; ++trial) {
        const int initial = 50 + static_cast<int>(rng() % 200U);
        IntRing* ring = MakeValueRing(initial);
        std::size_t length = static_cast<std::size_t>(initial);

        for (int step = 0; step < 300 && length > 0; ++step) {
            const std::vector<int> before = RingValues(ring);
            CONTAINER_REQUIRE(before.size() == length);
            const int k = static_cast<int>(rng() % (length * 2U + 3U));
            IntRing* removed = ring->Unlink(k);
            const std::size_t remove_count =
                (k <= 0) ? 0U : static_cast<std::size_t>(k) % length;

            if (k <= 0) {
                CONTAINER_CHECK(removed == nullptr);
                VerifyRingStructure(ring, length);
                continue;
            }

            CONTAINER_REQUIRE(removed != nullptr);
            if (remove_count == 0) {
                // n % Len == 0：环不变，返回值是原 Next()（不是新环，不能释放）。
                CONTAINER_CHECK(removed == ring->Next());
                VerifyRingStructure(ring, length);
                continue;
            }

            const std::vector<int> removed_values = RingValues(removed);
            CONTAINER_REQUIRE(removed_values.size() == remove_count);
            for (std::size_t i = 0; i < remove_count; ++i) {
                CONTAINER_CHECK(removed_values[i] == before[i + 1]);
            }
            IntRing::DeleteRing(removed);
            length -= remove_count;

            std::vector<int> expected_remaining;
            expected_remaining.push_back(before[0]);
            for (std::size_t i = remove_count + 1; i < before.size(); ++i) {
                expected_remaining.push_back(before[i]);
            }
            CONTAINER_CHECK(RingValues(ring) == expected_remaining);
            VerifyRingStructure(ring, length);
        }
        IntRing::DeleteRing(ring);
    }
}

// ---------------------------------------------------------------------------
// 5. Ring 随机 Link/Unlink：节点集合守恒
// ---------------------------------------------------------------------------
void VerifyRingIdSet(IntRing* main, const std::vector<IntRing*>& detached,
                     int expected_total) {
    std::vector<int> all = RingValues(main);
    for (IntRing* ring : detached) {
        const std::vector<int> part = RingValues(ring);
        all.insert(all.end(), part.begin(), part.end());
    }
    std::sort(all.begin(), all.end());
    CONTAINER_REQUIRE(all.size() == static_cast<std::size_t>(expected_total));
    for (int i = 0; i < expected_total; ++i) {
        CONTAINER_CHECK(all[static_cast<std::size_t>(i)] == i);
    }
}

void TestRingRandomLinkUnlink() {
    go2cpp_container_test::Announce("random: Ring Link/Unlink set conservation");

    std::mt19937 rng(1234U);
    const int total = 100;
    IntRing* main = MakeValueRing(total);
    CONTAINER_REQUIRE(main != nullptr);
    std::size_t main_len = static_cast<std::size_t>(total);
    std::vector<IntRing*> detached;

    for (int step = 0; step < 2000; ++step) {
        if (!detached.empty() && rng() % 100U < 45U) {
            const std::size_t index =
                static_cast<std::size_t>(rng() % detached.size());
            IntRing* ring = detached[index];
            const std::size_t ring_len = ring->Len();
            IntRing* returned = main->Link(ring);
            CONTAINER_CHECK(returned != nullptr);
            main_len += ring_len;
            detached[index] = detached.back();
            detached.pop_back();
        } else {
            const int k = static_cast<int>(rng() % (main_len * 2U + 3U));
            IntRing* removed = main->Unlink(k);
            if (k <= 0) {
                CONTAINER_CHECK(removed == nullptr);
            } else {
                CONTAINER_REQUIRE(removed != nullptr);
                const std::size_t remove_count =
                    static_cast<std::size_t>(k) % main_len;
                if (remove_count == 0) {
                    // 同一环内的引用，不构成新环。
                    CONTAINER_CHECK(removed == main->Next());
                } else {
                    detached.push_back(removed);
                    main_len -= remove_count;
                }
            }
        }
        VerifyRingStructure(main, main_len);
        VerifyRingIdSet(main, detached, total);
    }

    while (!detached.empty()) {
        IntRing* ring = detached.back();
        detached.pop_back();
        main_len += ring->Len();
        main->Link(ring);
    }
    CONTAINER_CHECK(main_len == static_cast<std::size_t>(total));
    CONTAINER_CHECK(RingValues(main).size() == static_cast<std::size_t>(total));
    VerifyRingStructure(main, static_cast<std::size_t>(total));
    IntRing::DeleteRing(main);
}

// ---------------------------------------------------------------------------
// 6. Ring 随机 Next/Prev 往返
// ---------------------------------------------------------------------------
void TestRingNextPrevRandomWalk() {
    go2cpp_container_test::Announce("random: Ring Next/Prev inverse walk");

    IntRing* ring = MakeValueRing(1000);
    CONTAINER_REQUIRE(ring != nullptr);
    std::mt19937 rng(555U);
    IntRing* node = ring;
    for (int i = 0; i < 200000; ++i) {
        const bool forward = (rng() % 2U) == 0U;
        IntRing* next = forward ? node->Next() : node->Prev();
        if (forward) {
            CONTAINER_CHECK(next->Prev() == node);
            CONTAINER_CHECK(node->Next() == next);
        } else {
            CONTAINER_CHECK(next->Next() == node);
            CONTAINER_CHECK(node->Prev() == next);
        }
        node = next;
    }
    CONTAINER_CHECK(ring->Len() == 1000);
    IntRing::DeleteRing(ring);
}

}  // namespace

void RunRandomTests() {
    TestTemplateHeapAgainstPriorityQueue();
    TestInterfaceHeapRandomFixRemove();
    TestListAgainstStdList();
    TestRingRandomUnlinkOracle();
    TestRingRandomLinkUnlink();
    TestRingNextPrevRandomWalk();
}
