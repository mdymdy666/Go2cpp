/**
 * @file sort.hpp
 * @brief Go `sort` 包的 C++ 对应实现（语义对齐）。
 *
 * @details 复用优先的结论：
 *   | 候选 | 结论 |
 *   |---|---|
 *   | `std::sort` / `std::stable_sort` / `std::is_sorted` / `std::lower_bound` | **采用为底层**：Go 的 `sort.Sort` 是不稳定排序（Go 1.19+ 为 pdqsort）、`sort.Stable` 是稳定排序（symmerge），与 C++ 的 `std::sort`（introsort，不稳定）/ `std::stable_sort`（稳定）在**语义上正好对应** |
 *   | [pdqsort](https://github.com/orlp/pdqsort) 等第三方实现 | **不采用**：引入它是为了性能而非语义，而性能结论必须由 `build-bench`（Release）实测决定；先复用标准库，若基准显示热点再评估 |
 *
 *   **接口形态的映射**：Go 用 `sort.Interface`（`Len`/`Less`/`Swap` 三方法）表达「可排序」；
 *   C++ 的等价物是「迭代器/容器 + 比较器」，因此本模块用**模板 + 比较器**表达同一语义，
 *   不引入虚接口（避免为每次比较付出间接调用），但保留与 Go 相同的函数名与结果语义。
 *
 *   语义对照来源：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/sort/`
 *   （Go 1.23.0，BSD-3-Clause），**独立实现**。已对齐的非显然语义：
 *     - `sort.Search(n, f)` 返回**最小的** `i ∈ [0, n]` 使 `f(i)` 为真；全为假时返回 `n`
 *       （即「未找到」用 `n` 表达，而不是 -1）；
 *     - `sort.Find(n, cmp)` 用三向比较：`cmp(i) == 0` 即命中，返回 `(i, true)`；
 *       未命中返回 `(插入位置, false)`；
 *     - `SearchInts/Strings/Float64s` 都要求**切片已升序**，未排序时结果未定义（与 Go 文档一致）；
 *     - `Float64s` 的排序把 `NaN` 视为**小于任何其它值**（Go 的 `Float64Slice.Less`
 *       对 NaN 有专门处理），本实现对齐这一点。
 *
 * @note **边界**：Go 的 `sort.Reverse` 返回一个包装了 `Interface` 的接口值，C++ 侧对应物是
 *       `ReverseLess` 比较器（用法：`Sort(v, ReverseLess<std::less<int>>{})`），形态不同但语义一致；
 *       未实现 `sort.Interface` 的具体类型 `IntSlice`/`Float64Slice`/`StringSlice`（C++ 用容器 + 比较器）。
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * ## 模块定位
 *
 * Go 标准库适配层里的**纯计算模块**，零依赖（不碰 error、调度器、IO）。上游是
 * `go/src/sort/` 的函数契约，下游是「把 Go 源码逐行转译成 C++」的调用方 ——
 * **当前仓库内还没有业务调用方**（`tests/` 下也没有 sort 测试），所以本文件的价值
 * 全在 API 形状与语义对齐，而不是被某个运行时模块使用。
 *
 * ## 本文件定义什么
 *
 * - `Sort` / `Stable` / `IsSorted`：对应 `sort.Sort` / `sort.Stable` / `sort.IsSorted`，
 *   各有迭代器版与容器版重载；
 * - `Slice` / `SliceStable` / `SliceIsSorted`：对应 `sort.Slice*`，`less` 接收**下标**；
 * - `Ints` / `Strings` / `Float64s` 及 `*AreSorted`：常用类型的薄封装；
 * - `Search` / `Find` / `SearchInts` / `SearchStrings` / `SearchFloat64s`：二分搜索；
 * - `Float64LessNaN` / `ReverseLess` / `Reverse`：两个比较器装饰器。
 *
 * ## 阅读建议
 *
 * 先读文件头「接口形态的映射」理解为什么用模板 + 比较器而不是 `sort.Interface`；
 * 再读 `Float64LessNaN`（唯一不能直接转发标准库的地方）；最后读 `Search` / `Find`
 * 的「前假后真」契约 —— 用错契约时二分不会报错，只会给出看似合理的错误下标。
 *
 * ## 边界与陷阱
 *
 * - **稳定性由调用点决定**：`Sort` 对应 `std::sort`（不稳定），`Stable` 对应
 *   `std::stable_sort`（稳定）。这不是运行期可切换的状态；一旦用 `Sort` 排过，
 *   相等元素的原始相对顺序已经丢失，事后无法补救。
 * - `Slice*` 的 `less` 拿到的是「与 `values.data()` 的指针差」：回调里若让 `values`
 *   扩容/删除（换缓冲区），基址失效，后续下标全是错的（UB）。回调只应读元素。
 * - `Float64s` / `Float64sAreSorted` 必须用 `Float64LessNaN`：`double` 的 `operator<`
 *   对 NaN 恒为 false，直接排序会得到未定义序。
 * - `Search` 的谓词必须是「前假后真」；不满足时返回值无意义（与 Go 一样不做校验）。
 * - `SearchInts` / `SearchStrings` / `SearchFloat64s` 要求输入**已升序**，否则结果未定义。
 */

namespace go2cpp::sort {

// ============================================================================
// 排序（对应 sort.Sort / sort.Stable / sort.IsSorted）
// ============================================================================

/// 对应 `sort.Sort`（**不稳定**排序）。
template <typename Iterator, typename Less>
inline void Sort(Iterator first, Iterator last, Less less) {
    std::sort(first, last, less);
}

/// 便捷重载：对容器整体排序。
template <typename Container, typename Less>
inline void Sort(Container& container, Less less) {
    std::sort(std::begin(container), std::end(container), less);
}

/// 对应 `sort.Stable`（**稳定**排序：相等元素的相对顺序不变）。
template <typename Iterator, typename Less>
inline void Stable(Iterator first, Iterator last, Less less) {
    std::stable_sort(first, last, less);
}

/// 便捷重载：对容器整体做稳定排序。
template <typename Container, typename Less>
inline void Stable(Container& container, Less less) {
    std::stable_sort(std::begin(container), std::end(container), less);
}

/// 对应 `sort.IsSorted`。
template <typename Iterator, typename Less>
inline bool IsSorted(Iterator first, Iterator last, Less less) {
    return std::is_sorted(first, last, less);
}

/// 便捷重载。
template <typename Container, typename Less>
inline bool IsSorted(const Container& container, Less less) {
    return std::is_sorted(std::begin(container), std::end(container), less);
}

// ============================================================================
// 泛型切片排序（对应 sort.Slice / SliceStable / SliceIsSorted）
// ============================================================================

/**
 * @brief 对应 `sort.Slice(x, less)`：`less(i, j)` 接收**下标**。
 * @details Go 用下标而不是迭代器，这里保持同样形态，便于逐行转译 Go 代码。
 */
template <typename T, typename Less>
inline void Slice(std::vector<T>& values, Less less) {
    T* const base = values.data();          // 提前取基址：lambda 需要它来换算下标
    std::sort(values.begin(), values.end(), [&less, base](const T& left, const T& right) {
        return less(static_cast<std::int64_t>(&left - base),
                    static_cast<std::int64_t>(&right - base));
    });
}

/// 对应 `sort.SliceStable`。
template <typename T, typename Less>
inline void SliceStable(std::vector<T>& values, Less less) {
    T* const base = values.data();
    std::stable_sort(values.begin(), values.end(), [&less, base](const T& left, const T& right) {
        return less(static_cast<std::int64_t>(&left - base),
                    static_cast<std::int64_t>(&right - base));
    });
}

/// 对应 `sort.SliceIsSorted`。
template <typename T, typename Less>
inline bool SliceIsSorted(const std::vector<T>& values, Less less) {
    const T* const base = values.data();
    return std::is_sorted(values.begin(), values.end(), [&less, base](const T& left, const T& right) {
        return less(static_cast<std::int64_t>(&left - base),
                    static_cast<std::int64_t>(&right - base));
    });
}

// ============================================================================
// 常用类型的便捷排序（对应 Ints / Float64s / Strings 及 *AreSorted）—— 薄封装
// ============================================================================
//
// 这些函数内部就是一行标准库调用（**不重写算法**），存在的价值是让 Go 源里的
// `sort.Ints(x)` 在转译后仍有一一对应的 API 名，而不是要求转译器硬编码 `std::sort`。
//
//   sort.Ints(x)              ->  std::sort(x.begin(), x.end());
//   sort.IntsAreSorted(x)     ->  std::is_sorted(x.begin(), x.end());
//   sort.Strings(x)           ->  std::sort(x.begin(), x.end());        // std::string 可比较
//   sort.StringsAreSorted(x)  ->  std::is_sorted(x.begin(), x.end());
//   sort.Float64s(x)          ->  std::sort(x.begin(), x.end(), Float64LessNaN{});   // ★ 差别在这
//   sort.Float64sAreSorted(x) ->  std::is_sorted(x.begin(), x.end(), Float64LessNaN{});
//
// **严谨考虑过的差别**：浮点排序上 Go 的 `lessFloat64` 把 **NaN 视为最小**，
// 而 C++ 的 `operator<` 对 NaN 恒为 false（会得到未定义序）。因此这里必须显式传入
// `Float64LessNaN`，这也是唯一不能"直接转发"的地方。

/** @brief 对应 Go 的 `lessFloat64`：NaN 视为**小于**任何其它值（因此排在最前）。 */
/**
 * ## 定位
 *
 * 供 `Float64s` / `Float64sAreSorted` 使用；是 `double` 排序能对齐 Go 的唯一入口。
 *
 * ## 不变量
 *
 * - 构成严格弱序：NaN 与自身比较为 false，NaN 小于任何非 NaN，非 NaN 按 `<` 比较。
 * - 所有 NaN（含不同 payload、`-NaN`）在该序下互相等价，都排在最前。
 */
struct Float64LessNaN {
    bool operator()(double left, double right) const {
        if (std::isnan(left)) return !std::isnan(right);
        if (std::isnan(right)) return false;
        return left < right;
    }
};

/// 对应 `sort.Ints`（薄封装：一行 `std::sort`）。
inline void Ints(std::vector<int>& values) { std::sort(values.begin(), values.end()); }

/// 对应 `sort.IntsAreSorted`。
inline bool IntsAreSorted(const std::vector<int>& values) {
    return std::is_sorted(values.begin(), values.end());
}

/// 对应 `sort.Strings`。
inline void Strings(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
}

/// 对应 `sort.StringsAreSorted`。
inline bool StringsAreSorted(const std::vector<std::string>& values) {
    return std::is_sorted(values.begin(), values.end());
}

/// 对应 `sort.Float64s`（**必须**用 `Float64LessNaN`，见上面的差别说明）。
inline void Float64s(std::vector<double>& values) {
    std::sort(values.begin(), values.end(), Float64LessNaN{});
}

/// 对应 `sort.Float64sAreSorted`。
inline bool Float64sAreSorted(const std::vector<double>& values) {
    return std::is_sorted(values.begin(), values.end(), Float64LessNaN{});
}

// ============================================================================
// 搜索（对应 sort.Search / Find / SearchInts / SearchStrings / SearchFloat64s）
// ============================================================================

/**
 * @brief 对应 `sort.Search(n, f)`：二分查找**最小的** `i ∈ [0, n]` 使 `f(i)` 为真。
 * @details `f` 必须是「前假后真」的单调谓词；全为假时返回 `n`（Go 用 `n` 表示未找到）。
 */
/**
 * @brief 返回最小的 `i ∈ [0, n]` 使 `predicate(i)` 为真（Go `sort.Search`）。
 *
 * ## 步骤
 * step1. 维护半开区间 `[low, high)`，初值 `[0, n]`，不变量是「答案必在区间内」；
 * step2. 取中点 `middle = low + (high-low)/2`（这样写避免 `low+high` 溢出）；
 * step3. 谓词为真说明答案 `<= middle`，收缩右界；为假说明答案 `> middle`，左界右移；
 * step4. 区间为空时 `low` 就是答案 —— 全假时为 `n`，即 Go 的「未找到」表示法。
 *
 * ## 边界
 * 谓词必须满足「存在切分点，之前全假、之后全真」；否则返回值无意义（不校验）。
 * 谓词被调用 O(log n) 次，不要在里面做重活。
 */
template <typename Predicate>
inline std::int64_t Search(std::int64_t n, Predicate predicate) {
    // step1: 半开区间 [low, high)，答案必在其中。
    std::int64_t low = 0;
    std::int64_t high = n;
    while (low < high) {
        // step2: 防溢出的中点。
        const std::int64_t middle = low + (high - low) / 2;
        // step3: 真则右界收缩，假则左界右移。
        if (predicate(middle)) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    // step4: 区间空，low 即最小满足位置（全假时为 n）。
    return low;
}

/**
 * @brief 对应 `sort.Find(n, cmp)`：三向比较版本，返回 `(下标, 是否命中)`。
 * @details 未命中时返回插入位置与 `false`（与 Go 一致）。
 */
/**
 * @brief 三向比较的二分查找，返回 `(下标, 是否命中)`（Go `sort.Find`）。
 *
 * ## 步骤
 * step1. 半开区间 `[low, high)`，初值 `[0, n]`；
 * step2. 取中点，调用 `compare(middle)`：`0` 表示命中，立即返回 `(middle, true)`；
 * step3. 返回负数表示「目标在右侧」，`low = middle + 1`；返回正数表示「在左侧」，
 *        `high = middle`；
 * step4. 区间空且未命中时返回 `(low, false)`，`low` 就是目标的插入位置。
 *
 * ## 边界
 * `compare` 必须按同一全序返回符号；`n == 0` 时返回 `(0, false)`。
 */
template <typename Compare>
inline std::pair<std::int64_t, bool> Find(std::int64_t n, Compare compare) {
    // step1: 半开区间 [low, high)。
    std::int64_t low = 0;
    std::int64_t high = n;
    while (low < high) {
        // step2: 命中直接返回。
        const std::int64_t middle = low + (high - low) / 2;
        const int ordering = compare(middle);
        if (ordering == 0) return {middle, true};
        // step3: 按比较结果的符号收缩区间。
        if (ordering < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    // step4: 未命中，low 是插入位置。
    return {low, false};
}

/// 对应 `sort.SearchInts`（要求 `values` 已升序）。
inline std::int64_t SearchInts(const std::vector<int>& values, int target) {
    const auto found = Search(static_cast<std::int64_t>(values.size()),
                              [&values, target](std::int64_t index) {
                                  return values[static_cast<std::size_t>(index)] >= target;
                              });
    return found;
}

/// 对应 `sort.SearchStrings`（要求 `values` 已升序）。
inline std::int64_t SearchStrings(const std::vector<std::string>& values,
                                  std::string_view target) {
    return Search(static_cast<std::int64_t>(values.size()),
                  [&values, target](std::int64_t index) {
                      return values[static_cast<std::size_t>(index)] >= target;
                  });
}

/// 对应 `sort.SearchFloat64s`（要求 `values` 已升序）。
inline std::int64_t SearchFloat64s(const std::vector<double>& values, double target) {
    return Search(static_cast<std::int64_t>(values.size()),
                  [&values, target](std::int64_t index) {
                      return values[static_cast<std::size_t>(index)] >= target;
                  });
}

// ============================================================================
// 反向比较器（对应 sort.Reverse）
// ============================================================================

/**
 * @brief 对应 `sort.Reverse` 的 C++ 形态：把比较器取反。
 * @note Go 的 `sort.Reverse(data)` 返回包装后的 `Interface`；C++ 里没有接口值，
 *       因此改为「比较器装饰器」，用法：`Sort(v, ReverseLess<std::less<int>>{})`。
 */
/**
 * ## 定位
 *
 * `sort.Reverse` 的 C++ 替身：把任意比较器取反后交给 `Sort` / `Stable` /
 * `Search` 等模板。当前仓库内没有调用方，供 Go 代码转译使用。
 *
 * ## 不变量
 *
 * - 若 `less` 是严格弱序，则 `ReverseLess` 也是严格弱序（等价类不变、方向反转），
 *   因此可以安全地传给 `std::sort` / `std::stable_sort` / `std::is_sorted`。
 * - `less` 按值持有；有状态的比较器会被复制，修改原对象不会影响已构造的装饰器。
 */
template <typename Less>
struct ReverseLess {
    Less less{};  ///< 被装饰的比较器；只读，由 operator() 以 (right,left) 顺序调用
    template <typename T>
    bool operator()(const T& left, const T& right) const {
        return less(right, left);
    }
};

/// 便捷构造（对应 `sort.Reverse`）。
template <typename Less>
inline ReverseLess<Less> Reverse(Less less) {
    return ReverseLess<Less>{less};
}

}  // namespace go2cpp::sort
