/**
 * @file test_ring.cpp
 * @brief `container::Ring` 的单元测试：逐项移植 Go `container/ring` 官方测试。
 *
 * ## 覆盖
 * 1. 空环 / 单元素环（含栈上零值环）；
 * 2. `New` / `Len` / `Next` / `Prev` / `Move` 的连接关系与模运算语义；
 * 3. `Link`：不同环拼接、`nullptr` no-op、**同环自链接切出子环**；
 * 4. `Unlink`：`n <= 0`、`n % Len == 0`、普通摘取子环；
 * 5. `Do` 的正向顺序（从任意起点）；
 * 6. `DeleteRing` 与单节点析构摘链（C++ 侧内存语义）。
 */
#include "container_test_support.hpp"

#include "go2cpp/container/ring.hpp"

#include <cstddef>
#include <vector>

namespace container = go2cpp::container;

namespace {

using IntRing = container::Ring<int>;

/** @brief 创建 n 个节点、值依次为 1..n 的环；n <= 0 返回 nullptr。 */
IntRing* MakeN(int n) {
    IntRing* ring = IntRing::New(static_cast<std::size_t>(n < 0 ? 0 : n));
    if (ring == nullptr) {
        return nullptr;
    }
    IntRing* node = ring;
    for (int i = 1; i <= n; ++i) {
        node->Value = i;
        node = node->Next();
    }
    return ring;
}

/** @brief 从 ring 起点按 Next 顺序收集 Value。 */
std::vector<int> Values(IntRing* ring) {
    std::vector<int> out;
    if (ring != nullptr) {
        ring->Do([&out](int& value) { out.push_back(value); });
    }
    return out;
}

/** @brief 环内所有 Value 之和。 */
int Sum(IntRing* ring) {
    int sum = 0;
    if (ring != nullptr) {
        ring->Do([&sum](int& value) { sum += value; });
    }
    return sum;
}

int SumN(int n) { return (n * n + n) / 2; }

/**
 * @brief 移植 Go ring_test.go 的 verify：长度、遍历、连接关系、Move。
 * @param expected_sum 负数表示跳过求和校验（Go 测试里的 -1）。
 */
void Verify(IntRing* ring, int expected_count, int expected_sum) {
    if (ring == nullptr) {
        CONTAINER_CHECK(expected_count == 0);
        return;
    }

    CONTAINER_CHECK(static_cast<int>(ring->Len()) == expected_count);

    int count = 0;
    int sum = 0;
    ring->Do([&count, &sum](int& value) {
        ++count;
        sum += value;
    });
    CONTAINER_CHECK(count == expected_count);
    if (expected_sum >= 0) {
        CONTAINER_CHECK(sum == expected_sum);
    }

    // 每个节点的 Next()->Prev() 必须指回自己，Prev()->Next() 同理。
    IntRing* node = ring;
    for (int i = 0; i < expected_count; ++i) {
        CONTAINER_REQUIRE(node->Next() != nullptr);
        CONTAINER_REQUIRE(node->Prev() != nullptr);
        CONTAINER_CHECK(node->Next()->Prev() == node);
        CONTAINER_CHECK(node->Prev()->Next() == node);
        node = node->Next();
    }
    CONTAINER_CHECK(node == ring);

    CONTAINER_CHECK(ring->Next() != nullptr);
    CONTAINER_CHECK(ring->Prev() != nullptr);
    CONTAINER_CHECK(ring->Move(0) == ring);
    CONTAINER_CHECK(ring->Move(expected_count) == ring);
    CONTAINER_CHECK(ring->Move(-expected_count) == ring);
    for (int i = 0; i < 10; ++i) {
        const int step = expected_count + i;
        CONTAINER_CHECK(ring->Move(step) == ring->Move(i));
        CONTAINER_CHECK(ring->Move(-step) == ring->Move(-i));
    }
}

// ---------------------------------------------------------------------------
// 1. 角落：nil / 零值单元素 / Link(nil) / Unlink(0)
// ---------------------------------------------------------------------------
void TestCornerCases() {
    go2cpp_container_test::Announce("ring: corner cases (nil / single)");

    Verify(nullptr, 0, 0);

    IntRing single;
    Verify(&single, 1, 0);
    single.Link(nullptr);  // nil 是 no-op
    Verify(&single, 1, 0);
    CONTAINER_CHECK(single.Unlink(0) == nullptr);
    Verify(&single, 1, 0);
}

// ---------------------------------------------------------------------------
// 2. New / Len / Next / Prev / Move
// ---------------------------------------------------------------------------
void TestNewAndMove() {
    go2cpp_container_test::Announce("ring: New / Len / Move");

    for (int n = 0; n <= 9; ++n) {
        IntRing* ring = IntRing::New(static_cast<std::size_t>(n));
        Verify(ring, n, -1);
        IntRing::DeleteRing(ring);
    }

    for (int n = 0; n <= 9; ++n) {
        IntRing* ring = MakeN(n);
        Verify(ring, n, n == 0 ? 0 : SumN(n));
        IntRing::DeleteRing(ring);
    }

    IntRing* ring = MakeN(10);
    CONTAINER_REQUIRE(ring != nullptr);
    CONTAINER_CHECK(ring->Move(1)->Value == 2);
    CONTAINER_CHECK(ring->Move(-1)->Value == 10);
    CONTAINER_CHECK(ring->Move(10)->Value == 1);
    CONTAINER_CHECK(ring->Move(-10)->Value == 1);
    IntRing::DeleteRing(ring);
}

// ---------------------------------------------------------------------------
// 3. Link：对应 Go TestLink1 / TestLink2 / TestLink3
// ---------------------------------------------------------------------------
void TestLink1() {
    go2cpp_container_test::Announce("ring: Link single elements (Go TestLink1)");

    IntRing* r1a = MakeN(1);
    IntRing* r1b = new IntRing();  // 零值：Value=0 的单元素环
    IntRing* r2a = r1a->Link(r1b);
    Verify(r2a, 2, 1);
    CONTAINER_CHECK(r2a == r1a);

    IntRing* r2b = r2a->Link(r2a->Next());
    Verify(r2b, 2, 1);
    CONTAINER_CHECK(r2b == r2a->Next());

    // 同环自链接：r 与 s 之间的元素被切出，形成两个独立的环。
    IntRing* r1c = r2b->Link(r2b);
    Verify(r1c, 1, 1);
    Verify(r2b, 1, 0);

    IntRing::DeleteRing(r1c);
    IntRing::DeleteRing(r2b);
}

void TestLink2() {
    go2cpp_container_test::Announce("ring: Link across rings (Go TestLink2)");

    IntRing* r1a = new IntRing(42);
    IntRing* r1b = new IntRing(77);
    IntRing* r10 = MakeN(10);

    CONTAINER_CHECK(r1a->Link(nullptr) != nullptr);
    Verify(r1a, 1, 42);

    r1a->Link(r1b);
    Verify(r1a, 2, 42 + 77);

    r10->Link(nullptr);
    Verify(r10, 10, SumN(10));

    IntRing* returned = r10->Link(r1a);
    Verify(r10, 12, SumN(10) + 42 + 77);
    // Link 返回链接前的 r10.Next()，其值应为 2；s 的整个环插在 r10 之后。
    CONTAINER_REQUIRE(returned != nullptr);
    CONTAINER_CHECK(returned->Value == 2);
    CONTAINER_CHECK(returned == r10->Move(3));

    // 顺序：1、42、77，然后才是 r10 原来的 2..10。
    const std::vector<int> order = Values(r10);
    CONTAINER_REQUIRE(order.size() == 12);
    CONTAINER_CHECK(order[0] == 1);
    CONTAINER_CHECK(order[1] == 42);
    CONTAINER_CHECK(order[2] == 77);
    for (int i = 2; i <= 10; ++i) {
        CONTAINER_CHECK(order[static_cast<std::size_t>(i + 1)] == i);
    }

    IntRing::DeleteRing(r10);
}

void TestLink3() {
    go2cpp_container_test::Announce("ring: repeated Link (Go TestLink3)");

    IntRing* root = new IntRing();
    int count = 1;
    for (int i = 1; i < 10; ++i) {
        count += i;
        root->Link(IntRing::New(static_cast<std::size_t>(i)));
        Verify(root, count, -1);
    }
    IntRing::DeleteRing(root);
}

// ---------------------------------------------------------------------------
// 4. Unlink：对应 Go TestUnlink / TestLinkUnlink
// ---------------------------------------------------------------------------
void TestUnlink() {
    go2cpp_container_test::Announce("ring: Unlink (Go TestUnlink)");

    IntRing* r10 = MakeN(10);
    IntRing* s10 = r10->Move(6);
    const int total = SumN(10);

    Verify(r10, 10, total);
    Verify(s10, 10, total);

    CONTAINER_CHECK(r10->Unlink(0) == nullptr);
    Verify(r10, 10, total);

    IntRing* r1 = r10->Unlink(1);  // 摘掉 r10.Next()：值 2
    Verify(r1, 1, 2);
    Verify(r10, 9, total - 2);
    CONTAINER_CHECK(Values(r1) == std::vector<int>{2});

    // 9 % 9 == 0：不摘任何元素，返回值仍指向环内节点（与 Go 测试一致）。
    IntRing* r9 = r10->Unlink(9);
    Verify(r9, 9, total - 2);
    Verify(r10, 9, total - 2);
    CONTAINER_CHECK(r9->Value == 3);  // 环已旋转，指向 r10.Next()
    CONTAINER_CHECK(r9 == r10->Move(1));

    IntRing::DeleteRing(r1);
    IntRing::DeleteRing(r10);
}

void TestLinkUnlink() {
    go2cpp_container_test::Announce("ring: link/unlink round trip (Go TestLinkUnlink)");

    for (int i = 1; i < 4; ++i) {
        IntRing* ri = IntRing::New(static_cast<std::size_t>(i));
        for (int j = 0; j < i; ++j) {
            IntRing* rj = ri->Unlink(j);
            Verify(rj, j, -1);
            Verify(ri, i - j, -1);
            ri->Link(rj);
            Verify(ri, i, -1);
        }
        IntRing::DeleteRing(ri);
    }
}

// ---------------------------------------------------------------------------
// 5. Do：从任意起点按 Next 顺序
// ---------------------------------------------------------------------------
void TestDoOrder() {
    go2cpp_container_test::Announce("ring: Do traversal order");

    IntRing* ring = MakeN(5);
    CONTAINER_REQUIRE(ring != nullptr);
    CONTAINER_CHECK(Values(ring) == (std::vector<int>{1, 2, 3, 4, 5}));
    CONTAINER_CHECK(Values(ring->Move(3)) == (std::vector<int>{4, 5, 1, 2, 3}));

    // C++ 扩展：f 收到 T&，可以就地修改（Go 的 any 是值拷贝，修改不回写）。
    ring->Do([](int& value) { value *= 2; });
    CONTAINER_CHECK(Sum(ring) == 30);

    IntRing::DeleteRing(ring);
}

// ---------------------------------------------------------------------------
// 6. Unlink 返回值与 C++ 内存语义
// ---------------------------------------------------------------------------
void TestUnlinkSubring() {
    go2cpp_container_test::Announce("ring: Unlink returns removed subring");

    IntRing* ring = MakeN(10);
    IntRing* removed = ring->Unlink(3);  // 摘掉 2,3,4
    CONTAINER_REQUIRE(removed != nullptr);
    CONTAINER_CHECK(Values(removed) == (std::vector<int>{2, 3, 4}));
    CONTAINER_CHECK(Values(ring) == (std::vector<int>{1, 5, 6, 7, 8, 9, 10}));

    IntRing::DeleteRing(removed);
    IntRing::DeleteRing(ring);
}

void TestSingleNodeDestructor() {
    go2cpp_container_test::Announce("ring: deleting one node unlinks it");

    IntRing* ring = MakeN(3);  // 1, 2, 3
    CONTAINER_REQUIRE(ring != nullptr);
    IntRing* middle = ring->Next();      // 值 2
    IntRing* third = middle->Next();     // 值 3
    CONTAINER_CHECK(middle->Value == 2);
    delete middle;  // C++ 扩展：摘掉单节点，其余节点继续存活
    Verify(ring, 2, 1 + 3);
    CONTAINER_CHECK(ring->Next() == third);
    CONTAINER_CHECK(third->Prev() == ring);
    IntRing::DeleteRing(ring);
}

void TestDeleteRingNull() {
    go2cpp_container_test::Announce("ring: DeleteRing(nullptr)");
    IntRing::DeleteRing(nullptr);
    CONTAINER_CHECK(true);
}

}  // namespace

void RunRingTests() {
    TestCornerCases();
    TestNewAndMove();
    TestLink1();
    TestLink2();
    TestLink3();
    TestUnlink();
    TestLinkUnlink();
    TestDoOrder();
    TestUnlinkSubring();
    TestSingleNodeDestructor();
    TestDeleteRingNull();
}
