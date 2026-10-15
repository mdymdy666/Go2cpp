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
// 关于 Go 的 Ints / Float64s / Strings 及 *AreSorted（**不封装**）
// ============================================================================
//
// 这几个 Go API 在 C++ 里就是标准库的一行调用，再封装一层只会多一次没有价值的间接：
//
//   sort.Ints(x)              ->  std::sort(x.begin(), x.end());
//   sort.IntsAreSorted(x)     ->  std::is_sorted(x.begin(), x.end());
//   sort.Strings(x)           ->  std::sort(x.begin(), x.end());        // std::string 可比较
//   sort.StringsAreSorted(x)  ->  std::is_sorted(x.begin(), x.end());
//   sort.Float64s(x)          ->  std::sort(x.begin(), x.end(), Float64LessNaN{});
//   sort.Float64sAreSorted(x) ->  std::is_sorted(x.begin(), x.end(), Float64LessNaN{});
//
// 唯一带 Go 特有语义的是**浮点的 NaN 处理**（Go 的 lessFloat64 把 NaN 视为最小），
// 因此这里只保留一个比较器 `Float64LessNaN`，其余一律交给标准库。
//
//   （2026-10-15 按使用者反馈删除：原先实现的 Ints/Strings/IntsAreSorted/StringsAreSorted/
//     Float64s/Float64sAreSorted 六个函数都只是标准库的转发。）

/** @brief 对应 Go 的 `lessFloat64`：NaN 视为**小于**任何其它值（因此排在最前）。 */
struct Float64LessNaN {
    bool operator()(double left, double right) const {
        if (std::isnan(left)) return !std::isnan(right);
        if (std::isnan(right)) return false;
        return left < right;
    }
};

// ============================================================================
// 搜索（对应 sort.Search / Find / SearchInts / SearchStrings / SearchFloat64s）
// ============================================================================

/**
 * @brief 对应 `sort.Search(n, f)`：二分查找**最小的** `i ∈ [0, n]` 使 `f(i)` 为真。
 * @details `f` 必须是「前假后真」的单调谓词；全为假时返回 `n`（Go 用 `n` 表示未找到）。
 */
template <typename Predicate>
inline std::int64_t Search(std::int64_t n, Predicate predicate) {
    std::int64_t low = 0;
    std::int64_t high = n;
    while (low < high) {
        const std::int64_t middle = low + (high - low) / 2;
        if (predicate(middle)) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return low;
}

/**
 * @brief 对应 `sort.Find(n, cmp)`：三向比较版本，返回 `(下标, 是否命中)`。
 * @details 未命中时返回插入位置与 `false`（与 Go 一致）。
 */
template <typename Compare>
inline std::pair<std::int64_t, bool> Find(std::int64_t n, Compare compare) {
    std::int64_t low = 0;
    std::int64_t high = n;
    while (low < high) {
        const std::int64_t middle = low + (high - low) / 2;
        const int ordering = compare(middle);
        if (ordering == 0) return {middle, true};
        if (ordering < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
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
template <typename Less>
struct ReverseLess {
    Less less{};
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
