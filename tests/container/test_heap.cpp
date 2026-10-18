/**
 * @file test_heap.cpp
 * @brief `container::Heap` 的单元测试：接口式（对齐 Go）与模板式两种形态。
 *
 * ## 覆盖
 * 1. Go `container/heap` 官方测试的等价序列（Init / Push / Pop 交错）；
 * 2. `Remove` 的三种位置（末尾、堆顶、任意中位）与删除后的堆性质；
 * 3. `Fix`：优先级变大 / 变小后原地重排；
 * 4. 模板式 min-heap / max-heap / 自定义比较器 / 重复元素 / move-only 元素；
 * 5. 空堆、越界下标等边界（抛 `std::out_of_range`）。
 */
#include "container_test_support.hpp"

#include "go2cpp/container/heap.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace container = go2cpp::container;

namespace {

using go2cpp_container_test::IntVectorHeap;

/** @brief 校验接口式堆的 min-heap 性质：父节点不大于任一孩子。 */
void VerifyMinHeap(const IntVectorHeap& heap, const char* where) {
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
    (void)where;
}

/** @brief 把接口式堆全部弹出，返回升序序列（同时校验每一步的堆性质）。 */
std::vector<int> Drain(IntVectorHeap& heap) {
    std::vector<int> out;
    out.reserve(heap.Len());
    std::size_t guard = 0;
    while (heap.Len() > 0) {
        out.push_back(container::Pop(heap));
        VerifyMinHeap(heap, "drain");
        CONTAINER_REQUIRE(++guard <= 10000000);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 1. 等价 Go heap_test.go 的 TestInit0 / TestInit1 / Test
// ---------------------------------------------------------------------------
void TestGoInterfaceSequence() {
    go2cpp_container_test::Announce("heap: Go interface sequence");

    {
        IntVectorHeap heap;  // 全 0：重复元素
        for (int i = 20; i > 0; --i) {
            heap.Push(0);
        }
        container::Init(heap);
        VerifyMinHeap(heap, "init0");
        for (std::size_t i = 1; heap.Len() > 0; ++i) {
            const int value = container::Pop(heap);
            VerifyMinHeap(heap, "init0-pop");
            CONTAINER_CHECK(value == 0);
            CONTAINER_CHECK(i <= 20);
        }
    }

    {
        IntVectorHeap heap;  // 全不同：Init 必须把乱序数组整理成堆
        for (int i = 1; i <= 20; ++i) {
            heap.Push(21 - i);
        }
        container::Init(heap);
        VerifyMinHeap(heap, "init1");
        for (int expected = 1; heap.Len() > 0; ++expected) {
            CONTAINER_CHECK(container::Pop(heap) == expected);
            VerifyMinHeap(heap, "init1-pop");
        }
    }

    {
        // Go Test：先塞 20..11 再 Init，然后 free Push 10..1，最后边弹边压。
        IntVectorHeap heap;
        for (int i = 20; i > 10; --i) {
            heap.Push(i);
        }
        container::Init(heap);
        VerifyMinHeap(heap, "go-test-init");
        for (int i = 10; i > 0; --i) {
            container::Push(heap, i);
        }
        VerifyMinHeap(heap, "go-test-push");
        for (int i = 1; heap.Len() > 0; ++i) {
            const int value = container::Pop(heap);
            if (i < 20) {
                container::Push(heap, 20 + i);
            }
            VerifyMinHeap(heap, "go-test-pop");
            CONTAINER_CHECK(value == i);
        }
    }
}

// ---------------------------------------------------------------------------
// 2. Remove：末尾 / 堆顶 / 中间；每一步之后堆性质必须成立
// ---------------------------------------------------------------------------
void TestInterfaceRemove() {
    go2cpp_container_test::Announce("heap: Remove (tail / root / middle)");

    {
        IntVectorHeap heap;
        for (int i = 0; i < 10; ++i) {
            heap.Push(i);
        }
        container::Init(heap);
        for (int expected = 9; heap.Len() > 0; --expected) {
            const std::size_t last = heap.Len() - 1;
            const int value = container::Remove(heap, last);
            CONTAINER_CHECK(value == expected);
            VerifyMinHeap(heap, "remove-tail");
        }
    }

    {
        IntVectorHeap heap;
        for (int i = 0; i < 10; ++i) {
            heap.Push(i);
        }
        container::Init(heap);
        for (int expected = 0; heap.Len() > 0; ++expected) {
            CONTAINER_CHECK(container::Remove(heap, 0) == expected);
            VerifyMinHeap(heap, "remove-root");
        }
    }

    {
        IntVectorHeap heap;
        for (int i = 0; i < 10; ++i) {
            heap.Push(i);
        }
        container::Init(heap);
        std::vector<bool> seen(10, false);
        while (heap.Len() > 0) {
            const std::size_t middle = (heap.Len() - 1) / 2;
            const int value = container::Remove(heap, middle);
            CONTAINER_REQUIRE(value >= 0 && value < 10);
            CONTAINER_CHECK(!seen[static_cast<std::size_t>(value)]);
            seen[static_cast<std::size_t>(value)] = true;
            VerifyMinHeap(heap, "remove-middle");
        }
        for (int i = 0; i < 10; ++i) {
            CONTAINER_CHECK(seen[static_cast<std::size_t>(i)]);
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Fix：改大 / 改小 / 连续随机修改后仍能按序弹出
// ---------------------------------------------------------------------------
void TestInterfaceFix() {
    go2cpp_container_test::Announce("heap: Fix after priority change");

    IntVectorHeap heap;
    std::vector<int> reference;
    std::mt19937 rng(20261018U);
    for (int i = 0; i < 200; ++i) {
        const int value = static_cast<int>(rng() % 1000U);
        heap.Push(value);
        reference.push_back(value);
    }
    container::Init(heap);
    VerifyMinHeap(heap, "fix-init");

    for (int round = 0; round < 500; ++round) {
        const std::size_t index = static_cast<std::size_t>(rng() % heap.Len());
        const int old_value = heap.Data()[index];
        const int new_value = static_cast<int>(rng() % 1000U);
        heap.Data()[index] = new_value;
        container::Fix(heap, index);

        // 引用模型用 multiset 类操作：删一个旧值、插一个新值。
        const auto it = std::find(reference.begin(), reference.end(), old_value);
        CONTAINER_REQUIRE(it != reference.end());
        reference.erase(it);
        reference.push_back(new_value);
        VerifyMinHeap(heap, "fix-round");
    }

    const std::vector<int> drained = Drain(heap);
    std::sort(reference.begin(), reference.end());
    CONTAINER_CHECK(drained == reference);
}

// ---------------------------------------------------------------------------
// 4. 模板式：min-heap / max-heap / 自定义比较器 / 重复元素
// ---------------------------------------------------------------------------
void TestTemplateHeapBasics() {
    go2cpp_container_test::Announce("heap: template min/max + duplicates");

    std::mt19937 rng(42U);
    std::vector<int> values;
    values.reserve(20000);
    for (int i = 0; i < 20000; ++i) {
        values.push_back(static_cast<int>(rng() % 5000U));  // 有意制造重复
    }

    {
        container::Heap<int> heap;
        for (int value : values) {
            heap.Push(value);
        }
        CONTAINER_CHECK(heap.Size() == values.size());
        CONTAINER_CHECK(!heap.Empty());
        CONTAINER_CHECK(heap.Top() == *std::min_element(values.begin(), values.end()));

        std::vector<int> popped;
        popped.reserve(values.size());
        while (!heap.Empty()) {
            popped.push_back(heap.Pop());
        }
        std::sort(values.begin(), values.end());
        CONTAINER_CHECK(popped == values);
        CONTAINER_CHECK(heap.Empty());
        CONTAINER_CHECK(heap.Size() == 0);
    }

    {
        // max-heap：传 std::greater<> 后 Pop 返回最大值。
        std::vector<int> source = values;
        container::Heap<int, std::greater<>> heap;
        for (int value : source) {
            heap.Push(value);
        }
        std::sort(source.begin(), source.end(), std::greater<>());
        for (int expected : source) {
            CONTAINER_CHECK(heap.Pop() == expected);
        }
    }

    {
        // 重复元素 + 可写 Top（改完由用户自己保证堆性质，这里只验证读写）。
        container::Heap<int> heap;
        for (int i = 0; i < 64; ++i) {
            heap.Push(7);
        }
        CONTAINER_CHECK(heap.Top() == 7);
        heap.Top() = 3;
        CONTAINER_CHECK(heap.Top() == 3);
        for (int i = 0; i < 64; ++i) {
            CONTAINER_CHECK(heap.Pop() == (i == 0 ? 3 : 7));
        }
    }

    {
        container::Heap<std::string> heap;
        heap.Push(std::string("pear"));
        heap.Push(std::string("apple"));
        heap.Push(std::string("orange"));
        CONTAINER_CHECK(heap.Pop() == "apple");
        CONTAINER_CHECK(heap.Pop() == "orange");
        CONTAINER_CHECK(heap.Pop() == "pear");
    }
}

struct Task {
    int deadline;
    int id;
};

struct TaskLess {
    bool operator()(const Task& left, const Task& right) const {
        if (left.deadline != right.deadline) {
            return left.deadline < right.deadline;
        }
        return left.id < right.id;
    }
};

void TestTemplateCustomComparator() {
    go2cpp_container_test::Announce("heap: template custom comparator");

    container::Heap<Task, TaskLess> heap;
    const Task tasks[] = {
        {50, 1}, {10, 2}, {30, 3}, {10, 4}, {40, 5}, {20, 6},
    };
    for (const Task& task : tasks) {
        heap.Push(task);
    }
    heap.Push(Task{10, 0});  // 同 deadline，id 更小者优先

    int last_deadline = -1;
    int last_id = -1;
    while (!heap.Empty()) {
        const Task task = heap.Pop();
        CONTAINER_CHECK(task.deadline >= last_deadline);
        if (task.deadline == last_deadline) {
            CONTAINER_CHECK(task.id > last_id);
        }
        last_deadline = task.deadline;
        last_id = task.id;
    }
}

void TestTemplateMoveOnly() {
    go2cpp_container_test::Announce("heap: template move-only elements");

    struct Less {
        bool operator()(const std::unique_ptr<int>& left,
                        const std::unique_ptr<int>& right) const {
            return *left < *right;
        }
    };

    container::Heap<std::unique_ptr<int>, Less> heap;
    heap.Push(std::make_unique<int>(5));
    heap.Push(std::make_unique<int>(1));
    heap.Push(std::make_unique<int>(3));

    std::unique_ptr<int> first = heap.Pop();
    std::unique_ptr<int> second = heap.Pop();
    std::unique_ptr<int> third = heap.Pop();
    CONTAINER_REQUIRE(first != nullptr);
    CONTAINER_REQUIRE(second != nullptr);
    CONTAINER_REQUIRE(third != nullptr);
    CONTAINER_CHECK(*first == 1);
    CONTAINER_CHECK(*second == 3);
    CONTAINER_CHECK(*third == 5);
}

// ---------------------------------------------------------------------------
// 5. 边界：空堆 Pop/Top、越界 Fix/Remove
// ---------------------------------------------------------------------------
void TestBoundaries() {
    go2cpp_container_test::Announce("heap: empty / out-of-range boundaries");

    IntVectorHeap interface_heap;
    container::Init(interface_heap);  // 空容器 Init 是合法 no-op
    VerifyMinHeap(interface_heap, "empty-init");

    bool threw = false;
    try {
        (void)container::Pop(interface_heap);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CONTAINER_CHECK(threw);

    threw = false;
    try {
        (void)container::Remove(interface_heap, 0);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CONTAINER_CHECK(threw);

    threw = false;
    try {
        container::Fix(interface_heap, 0);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CONTAINER_CHECK(threw);

    container::Push(interface_heap, 7);
    threw = false;
    try {
        container::Fix(interface_heap, 1);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CONTAINER_CHECK(threw);
    CONTAINER_CHECK(container::Pop(interface_heap) == 7);

    container::Heap<int> template_heap;
    threw = false;
    try {
        (void)template_heap.Pop();
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CONTAINER_CHECK(threw);

    threw = false;
    try {
        (void)template_heap.Top();
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CONTAINER_CHECK(threw);

    template_heap.Push(1);
    template_heap.Push(2);
    template_heap.Clear();
    CONTAINER_CHECK(template_heap.Empty());
}

}  // namespace

void RunHeapTests() {
    TestGoInterfaceSequence();
    TestInterfaceRemove();
    TestInterfaceFix();
    TestTemplateHeapBasics();
    TestTemplateCustomComparator();
    TestTemplateMoveOnly();
    TestBoundaries();
}
