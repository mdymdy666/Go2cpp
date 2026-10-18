/**
 * @file test_list.cpp
 * @brief `container::List` 的单元测试：Go 语义 + `Element*` 指针稳定。
 *
 * ## 覆盖
 * 1. `PushFront` / `PushBack` / `Front` / `Back` / `Len` / `Next` / `Prev`；
 * 2. `InsertBefore` / `InsertAfter`（含非法 mark 返回 nullptr 且不改动）；
 * 3. `InsertValue` 的 0 / 中间 / 末尾 / 越界下标；
 * 4. `Remove`（本链表 / 外来元素 / nullptr）；
 * 5. `MoveToFront` / `MoveToBack` / `MoveBefore` / `MoveAfter` 与全部 no-op 分支；
 * 6. **指针稳定**：插入/删除其他元素后，已有 `Element*` 仍指向同一节点；
 * 7. `Clear` / `Init` 后复用、析构释放、非默认构造类型、`std::string`。
 */
#include "container_test_support.hpp"

#include "go2cpp/container/list.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace container = go2cpp::container;

namespace {

template <typename T>
std::vector<T> ForwardValues(container::List<T>& list) {
    std::vector<T> values;
    for (auto* node = list.Front(); node != nullptr; node = node->Next()) {
        values.push_back(node->Value);
    }
    return values;
}

template <typename T>
std::vector<T> BackwardValues(container::List<T>& list) {
    std::vector<T> values;
    for (auto* node = list.Back(); node != nullptr; node = node->Prev()) {
        values.push_back(node->Value);
    }
    return values;
}

template <typename T>
void CheckOrder(container::List<T>& list, const std::vector<T>& expected,
                const char* what) {
    (void)what;
    CONTAINER_CHECK(ForwardValues(list) == expected);
    std::vector<T> reversed(expected.rbegin(), expected.rend());
    CONTAINER_CHECK(BackwardValues(list) == reversed);
    CONTAINER_CHECK(list.Len() == expected.size());
}

// ---------------------------------------------------------------------------
// 1. 基本插入与遍历
// ---------------------------------------------------------------------------
void TestPushAndTraverse() {
    go2cpp_container_test::Announce("list: push front/back + traversal");

    container::List<int> list;
    CONTAINER_CHECK(list.Len() == 0);
    CONTAINER_CHECK(list.Front() == nullptr);
    CONTAINER_CHECK(list.Back() == nullptr);

    auto* one = list.PushBack(1);
    auto* two = list.PushBack(2);
    auto* zero = list.PushFront(0);
    CONTAINER_REQUIRE(one != nullptr);
    CONTAINER_REQUIRE(two != nullptr);
    CONTAINER_REQUIRE(zero != nullptr);

    CheckOrder(list, std::vector<int>{0, 1, 2}, "push");

    CONTAINER_CHECK(list.Front() == zero);
    CONTAINER_CHECK(list.Back() == two);
    CONTAINER_CHECK(zero->Next() == one);
    CONTAINER_CHECK(one->Next() == two);
    CONTAINER_CHECK(two->Next() == nullptr);
    CONTAINER_CHECK(two->Prev() == one);
    CONTAINER_CHECK(one->Prev() == zero);
    CONTAINER_CHECK(zero->Prev() == nullptr);
}

// ---------------------------------------------------------------------------
// 2. InsertBefore / InsertAfter / InsertValue
// ---------------------------------------------------------------------------
void TestInsert() {
    go2cpp_container_test::Announce("list: InsertBefore / InsertAfter / InsertValue");

    container::List<int> list;
    auto* e1 = list.PushBack(1);
    auto* e3 = list.PushBack(3);
    CONTAINER_REQUIRE(e1 != nullptr && e3 != nullptr);

    auto* e2 = list.InsertBefore(2, e3);
    CONTAINER_REQUIRE(e2 != nullptr);
    CheckOrder(list, std::vector<int>{1, 2, 3}, "insert-before");

    auto* e15 = list.InsertAfter(15, e1);
    CONTAINER_REQUIRE(e15 != nullptr);
    CheckOrder(list, std::vector<int>{1, 15, 2, 3}, "insert-after");

    // 非法 mark：不属于本链表 / nullptr -> 返回 nullptr 且不修改。
    container::List<int> other;
    auto* foreign = other.PushBack(99);
    CONTAINER_CHECK(list.InsertBefore(7, foreign) == nullptr);
    CONTAINER_CHECK(list.InsertAfter(7, foreign) == nullptr);
    CONTAINER_CHECK(list.InsertBefore(7, nullptr) == nullptr);
    CONTAINER_CHECK(list.InsertAfter(7, nullptr) == nullptr);
    CheckOrder(list, std::vector<int>{1, 15, 2, 3}, "insert-invalid-mark");
    CONTAINER_CHECK(other.Len() == 1);
    CONTAINER_CHECK(other.Front() == foreign);

    // InsertValue：0 / 中间 / Len / 越界。
    container::List<int> values;
    values.PushBack(10);
    values.PushBack(20);
    values.PushBack(30);
    auto* front = values.InsertValue(5, 0);
    CONTAINER_REQUIRE(front != nullptr);
    CheckOrder(values, std::vector<int>{5, 10, 20, 30}, "insert-value-0");

    values.InsertValue(25, 3);
    CheckOrder(values, std::vector<int>{5, 10, 20, 25, 30}, "insert-value-middle");

    values.InsertValue(35, values.Len());
    CheckOrder(values, std::vector<int>{5, 10, 20, 25, 30, 35}, "insert-value-len");

    values.InsertValue(40, values.Len() + 100);
    CheckOrder(values, std::vector<int>{5, 10, 20, 25, 30, 35, 40},
               "insert-value-past-end");
}

// ---------------------------------------------------------------------------
// 3. Remove 语义
// ---------------------------------------------------------------------------
void TestRemove() {
    go2cpp_container_test::Announce("list: Remove semantics");

    container::List<int> list;
    list.PushBack(1);
    auto* middle = list.PushBack(2);
    list.PushBack(3);
    CONTAINER_REQUIRE(middle != nullptr);

    const int removed = list.Remove(middle);
    CONTAINER_CHECK(removed == 2);
    CheckOrder(list, std::vector<int>{1, 3}, "remove-middle");

    // 外来元素：返回其值、不修改任何链表（Go 的 no-op 语义）。
    container::List<int> other;
    auto* foreign = other.PushBack(7);
    CONTAINER_CHECK(list.Remove(foreign) == 7);
    CONTAINER_CHECK(other.Len() == 1);
    CONTAINER_CHECK(other.Front() == foreign);
    CONTAINER_CHECK(foreign->Value == 7);

    // nullptr 在 Go 里会 panic，这里抛 invalid_argument。
    bool threw = false;
    try {
        (void)list.Remove(nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CONTAINER_CHECK(threw);

    // 删除到空，再复用。
    CONTAINER_CHECK(list.Remove(list.Front()) == 1);
    CONTAINER_CHECK(list.Remove(list.Front()) == 3);
    CONTAINER_CHECK(list.Front() == nullptr);
    CONTAINER_CHECK(list.Back() == nullptr);
    CONTAINER_CHECK(list.Len() == 0);
    list.PushBack(42);
    CheckOrder(list, std::vector<int>{42}, "remove-then-reuse");
}

// ---------------------------------------------------------------------------
// 4. Move 系列
// ---------------------------------------------------------------------------
void TestMoves() {
    go2cpp_container_test::Announce("list: MoveToFront/Back/Before/After");

    container::List<int> list;
    auto* e0 = list.PushBack(0);
    auto* e1 = list.PushBack(1);
    auto* e2 = list.PushBack(2);
    auto* e3 = list.PushBack(3);
    auto* e4 = list.PushBack(4);
    CONTAINER_REQUIRE(e0 && e1 && e2 && e3 && e4);

    list.MoveToFront(e4);
    CheckOrder(list, std::vector<int>{4, 0, 1, 2, 3}, "move-to-front");

    list.MoveToBack(e4);
    CheckOrder(list, std::vector<int>{0, 1, 2, 3, 4}, "move-to-back");

    list.MoveBefore(e4, e1);
    CheckOrder(list, std::vector<int>{0, 4, 1, 2, 3}, "move-before");

    list.MoveAfter(e0, e3);
    CheckOrder(list, std::vector<int>{4, 1, 2, 3, 0}, "move-after");

    // no-op：e == mark、已经在目标位置、外来元素。
    list.MoveBefore(e1, e1);
    list.MoveAfter(e1, e1);
    list.MoveBefore(e1, e1->Next());  // 已经在 mark 之前
    CheckOrder(list, std::vector<int>{4, 1, 2, 3, 0}, "move-noop");

    container::List<int> other;
    auto* foreign = other.PushBack(9);
    list.MoveToFront(foreign);
    list.MoveToBack(foreign);
    list.MoveBefore(foreign, e1);
    list.MoveAfter(foreign, e1);
    CheckOrder(list, std::vector<int>{4, 1, 2, 3, 0}, "move-foreign");
    CONTAINER_CHECK(other.Len() == 1);
    CONTAINER_CHECK(other.Front() == foreign);

    // 单元素链表的移动必须安全。
    container::List<int> single;
    auto* only = single.PushBack(1);
    single.MoveToFront(only);
    single.MoveToBack(only);
    CheckOrder(single, std::vector<int>{1}, "move-single");
}

// ---------------------------------------------------------------------------
// 5. 指针稳定（本模块存在的核心理由）
// ---------------------------------------------------------------------------
struct Payload {
    int id;
    std::string name;
};

void TestPointerStability() {
    go2cpp_container_test::Announce("list: Element* stability under churn");

    container::List<Payload> list;
    std::vector<container::Element<Payload>*> handles;
    handles.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        handles.push_back(list.PushBack(Payload{i, "item-" + std::to_string(i)}));
    }

    auto* anchor = handles[500];
    CONTAINER_REQUIRE(anchor != nullptr);
    const Payload anchor_copy = anchor->Value;

    // 大量在两端插入：绝不能搬移已有节点。
    for (int i = 0; i < 500; ++i) {
        list.PushFront(Payload{10000 + i, "front"});
    }
    for (int i = 0; i < 500; ++i) {
        list.PushBack(Payload{20000 + i, "back"});
    }

    // 删除三分之一的初始节点：只允许被删节点自身失效。
    for (std::size_t i = 0; i < handles.size(); i += 3) {
        list.Remove(handles[i]);
    }

    CONTAINER_CHECK(anchor == handles[500]);
    CONTAINER_CHECK(anchor->Value.id == anchor_copy.id);
    CONTAINER_CHECK(anchor->Value.name == anchor_copy.name);

    // 邻居关系必须自洽：anchor 的前后节点互相指回 anchor。
    CONTAINER_REQUIRE(anchor->Prev() != nullptr);
    CONTAINER_REQUIRE(anchor->Next() != nullptr);
    CONTAINER_CHECK(anchor->Prev()->Next() == anchor);
    CONTAINER_CHECK(anchor->Next()->Prev() == anchor);

    // 与手工模型对比完整顺序。
    std::vector<Payload> expected;
    for (int i = 499; i >= 0; --i) {
        expected.push_back(Payload{10000 + i, "front"});
    }
    for (int i = 0; i < 1000; ++i) {
        if (i % 3 != 0) {
            expected.push_back(Payload{i, "item-" + std::to_string(i)});
        }
    }
    for (int i = 0; i < 500; ++i) {
        expected.push_back(Payload{20000 + i, "back"});
    }
    CONTAINER_CHECK(list.Len() == expected.size());

    const std::vector<Payload> actual = ForwardValues(list);
    CONTAINER_REQUIRE(actual.size() == expected.size());
    bool same = true;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (actual[i].id != expected[i].id || actual[i].name != expected[i].name) {
            same = false;
            break;
        }
    }
    CONTAINER_CHECK(same);

    // 移动操作同样不能改变节点地址。
    const auto* address_before = anchor;
    list.MoveToFront(anchor);
    CONTAINER_CHECK(list.Front() == anchor);
    list.MoveToBack(anchor);
    CONTAINER_CHECK(list.Back() == anchor);
    list.MoveBefore(anchor, list.Front());
    CONTAINER_CHECK(anchor == address_before);
    CONTAINER_CHECK(anchor->Value.id == 500);
}

// ---------------------------------------------------------------------------
// 6. Clear / Init / 析构 / 非默认构造 / string
// ---------------------------------------------------------------------------
void TestClearAndLifetime() {
    go2cpp_container_test::Announce("list: Clear / Init / lifetime");

    container::List<int> list;
    for (int i = 0; i < 100; ++i) {
        list.PushBack(i);
    }
    list.Clear();
    CONTAINER_CHECK(list.Len() == 0);
    CONTAINER_CHECK(list.Front() == nullptr);
    CONTAINER_CHECK(list.Back() == nullptr);
    list.PushBack(7);
    CheckOrder(list, std::vector<int>{7}, "clear-reuse");

    list.Init();
    CONTAINER_CHECK(list.Len() == 0);
    CONTAINER_CHECK(list.Front() == nullptr);

    {
        container::List<std::string> strings;
        strings.PushBack(std::string("alpha"));
        strings.PushBack(std::string("beta"));
        strings.PushFront(std::string("zero"));
        CheckOrder(strings, std::vector<std::string>{"zero", "alpha", "beta"},
                   "strings");
    }  // 析构必须释放所有节点（valgrind/ASan 可验证）

    struct NoDefault {
        int value;
        explicit NoDefault(int v) : value(v) {}
    };
    container::List<NoDefault> no_default;
    auto* node = no_default.PushBack(NoDefault(5));
    CONTAINER_REQUIRE(node != nullptr);
    CONTAINER_CHECK(node->Value.value == 5);
    auto* inserted = no_default.InsertValue(NoDefault(1), 0);
    CONTAINER_REQUIRE(inserted != nullptr);
    CONTAINER_CHECK(no_default.Len() == 2);
    CONTAINER_CHECK(no_default.Front()->Value.value == 1);
    CONTAINER_CHECK(no_default.Back()->Value.value == 5);
    const NoDefault removed = no_default.Remove(node);
    CONTAINER_CHECK(removed.value == 5);
    CONTAINER_CHECK(no_default.Len() == 1);
}

}  // namespace

void RunListTests() {
    TestPushAndTraverse();
    TestInsert();
    TestRemove();
    TestMoves();
    TestPointerStability();
    TestClearAndLifetime();
}
