#include "go2cpp/error.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

// error 全面版：同一个组件的 7 个使用场景。
//   1  创建                errors::New 产出带消息的错误
//   2  ★ 包装与展开         errors::Wrap 建立错误链，Unwrap 逐层回退
//   3  ★ 沿链识别根因       errors::Is 能穿透多层包装命中最初的错误
//   4  nil 语义             errors::Is(err, nullptr) 等价于 Go 的 errors.Is(err, nil)
//   5  ★ 聚合               errors::Join 合并多条错误，可逐条取回
//   6  ★ 按类型提取         errors::As<T> 从错误链里取出指定类型的错误
//   7  ★ 薄封装等价性       errors:: 与既有 CamelCase API 行为一致
//
// 说明：error.hpp 新增的 errors:: 命名空间只是既有 NewError/Wrap/Join/Is/As
//       的薄封装，不改变错误对象的不可变与共享所有权约束。场景 7 专门验证这一点。

namespace {

using go2cpp::Error;
using go2cpp::ErrorPtr;

// 自定义错误类型，用于验证 As<T> 能按类型提取。
class TimeoutError final : public Error {
public:
    explicit TimeoutError(int milliseconds) : m_milliseconds(milliseconds) {}
    std::string Message() const override {
        return "operation timed out after " + std::to_string(m_milliseconds) + "ms";
    }
    int milliseconds() const noexcept { return m_milliseconds; }

private:
    int m_milliseconds{0};
};

bool scenario_create() {
    std::clog << "[full][场景1] errors::New 创建错误\n";
    const auto error = go2cpp::errors::New("something went wrong");
    const bool ok = error != nullptr &&
                    error->Message() == "something went wrong" &&
                    error->ErrorString() == "something went wrong" &&
                    error->Unwrap() == nullptr;
    std::cout << "  Message=\"" << (error ? error->Message() : "")
              << "\" Unwrap 为空=" << (error && error->Unwrap() == nullptr)
              << "（预期 something went wrong 1）\n";
    return ok;
}

bool scenario_wrap_and_unwrap() {
    std::clog << "[full][场景2] errors::Wrap 建链 + Unwrap 展开\n";
    const auto root = go2cpp::errors::New("disk full");
    const auto middle = go2cpp::errors::Wrap(root, "write failed");
    const auto outer = go2cpp::errors::Wrap(middle, "save config failed");

    // 从外到内逐层回退
    const auto level1 = go2cpp::errors::Unwrap(outer);
    const auto level2 = go2cpp::errors::Unwrap(level1);
    const auto level3 = go2cpp::errors::Unwrap(level2);

    // ★ 注意：Wrap 产出的错误，其 Message() 是「外层: 内层」的完整链
    //   （实测 "save config failed: write failed: disk full"），
    //   所以不能用 Message() 判断层级 —— 必须用身份（shared_ptr 指针）比较。
    const bool ok = outer != nullptr && level1 == middle && level2 == root &&
                    level3 == nullptr &&
                    root->Message() == "disk full" &&
                    outer->Message().rfind("save config failed", 0) == 0;
    std::cout << "  链=";
    ErrorPtr cursor = outer;
    while (cursor) {
        std::cout << "\"" << cursor->Message() << "\" ";
        cursor = go2cpp::errors::Unwrap(cursor);
    }
    std::cout << "（预期 三层，最内层是 disk full）\n";
    return ok;
}

bool scenario_is_through_chain() {
    std::clog << "[full][场景3] errors::Is 穿透多层包装命中根因\n";
    const auto root = go2cpp::errors::New("disk full");
    const auto wrapped = go2cpp::errors::Wrap(go2cpp::errors::Wrap(root, "write failed"),
                                              "save failed");
    const auto other = go2cpp::errors::New("unrelated");

    const bool matches_root = go2cpp::errors::Is(wrapped, root);
    const bool matches_self = go2cpp::errors::Is(root, root);
    const bool rejects_other = !go2cpp::errors::Is(wrapped, other);
    // 中间层对象本身不是链上节点的话不应命中
    const bool rejects_fresh = !go2cpp::errors::Is(wrapped, go2cpp::errors::New("disk full"));

    const bool ok = matches_root && matches_self && rejects_other && rejects_fresh;
    std::cout << "  命中根因=" << matches_root << " 命中自身=" << matches_self
              << " 无关错误不命中=" << rejects_other
              << " 同文本的新对象不命中=" << rejects_fresh << "（预期 1 1 1 1）\n";
    return ok;
}

bool scenario_nil_semantics() {
    std::clog << "[full][场景4] nil 语义：Is(err, nullptr)\n";
    const auto error = go2cpp::errors::New("boom");
    const ErrorPtr empty;

    // Go 里 errors.Is(err, nil) 仅在 err 为 nil 时为真
    const bool null_vs_null = go2cpp::errors::Is(empty, nullptr);
    const bool value_vs_null = go2cpp::errors::Is(error, nullptr);
    const bool null_vs_value = go2cpp::errors::Is(nullptr, error);

    const bool ok = null_vs_null && !value_vs_null && !null_vs_value;
    std::cout << "  Is(nil,nil)=" << null_vs_null << " Is(err,nil)=" << value_vs_null
              << " Is(nil,err)=" << null_vs_value << "（预期 1 0 0）\n";
    return ok;
}

bool scenario_join() {
    std::clog << "[full][场景5] errors::Join 聚合多条错误\n";
    const auto first = go2cpp::errors::New("primary failure");
    const auto second = go2cpp::errors::New("secondary failure");
    const auto third = go2cpp::errors::New("third failure");

    const auto joined = go2cpp::errors::Join({first, second, third});
    const bool created = joined != nullptr && !joined->Message().empty();

    // 每条都能在聚合结果里被识别
    const bool finds_first = go2cpp::errors::Is(joined, first);
    const bool finds_second = go2cpp::errors::Is(joined, second);
    const bool finds_third = go2cpp::errors::Is(joined, third);
    const bool rejects_absent =
        !go2cpp::errors::Is(joined, go2cpp::errors::New("not included"));

    // vector 重载与 initializer_list 重载应产出等价结果
    std::vector<ErrorPtr> list{first, second};
    const auto from_vector = go2cpp::errors::Join(list);
    const bool vector_ok = go2cpp::errors::Is(from_vector, first) &&
                           go2cpp::errors::Is(from_vector, second);

    const bool ok = created && finds_first && finds_second && finds_third &&
                    rejects_absent && vector_ok;
    std::cout << "  聚合消息=\"" << (joined ? joined->Message() : "") << "\"\n"
              << "  能识别三条成员=" << (finds_first && finds_second && finds_third)
              << " 无关错误不命中=" << rejects_absent
              << " vector 重载等价=" << vector_ok << "（预期 1 1 1）\n";
    return ok;
}

bool scenario_as_by_type() {
    std::clog << "[full][场景6] errors::As<T> 按类型提取\n";
    const auto timeout = std::make_shared<const TimeoutError>(250);
    const auto wrapped = go2cpp::errors::Wrap(timeout, "request failed");

    // 从包装后的错误里取出 TimeoutError
    const auto extracted = go2cpp::errors::As<TimeoutError>(wrapped);
    const bool extracted_ok = extracted != nullptr && extracted->milliseconds() == 250;

    // 类型不匹配时返回空
    const auto plain = go2cpp::errors::New("just text");
    const auto not_found = go2cpp::errors::As<TimeoutError>(plain);
    const bool rejects_wrong_type = not_found == nullptr;

    // 同一个错误自身也能被提取
    const auto direct = go2cpp::errors::As<TimeoutError>(timeout);
    const bool direct_ok = direct != nullptr && direct->milliseconds() == 250;

    const bool ok = extracted_ok && rejects_wrong_type && direct_ok;
    std::cout << "  从包装中提取超时错误=" << extracted_ok
              << " 毫秒=" << (extracted ? extracted->milliseconds() : -1)
              << " 类型不符返回空=" << rejects_wrong_type
              << "（预期 1 250 1）\n";
    return ok;
}

bool scenario_thin_wrapper_equivalence() {
    std::clog << "[full][场景7] 薄封装等价性：errors:: 与既有 CamelCase API 行为一致\n";
    using namespace go2cpp;

    // ① New 与 NewError
    const auto lower = errors::New("same message");
    const auto camel = NewError("same message");
    const bool new_equivalent = lower && camel &&
                                lower->Message() == camel->Message() &&
                                lower->ErrorString() == camel->ErrorString();

    // ② Wrap 与 Wrap：两条链的 Is 结果一致
    const auto lower_root = errors::New("root cause");
    const auto camel_root = NewError("root cause");
    const auto lower_wrapped = errors::Wrap(errors::Wrap(lower_root, "mid"), "outer");
    const auto camel_wrapped = Wrap(Wrap(camel_root, "mid"), "outer");
    const bool wrap_equivalent =
        errors::Is(lower_wrapped, lower_root) && Is(camel_wrapped, camel_root) &&
        lower_wrapped->Message() == camel_wrapped->Message();

    // ③ Is 与 Is：交叉验证（用小写 API 建链、大写 API 判定，反之亦然）
    const bool cross_1 = Is(lower_wrapped, lower_root);
    const bool cross_2 = errors::Is(camel_wrapped, camel_root);
    const bool cross_3 = Is(camel_wrapped, camel_root) ==
                         errors::Is(camel_wrapped, camel_root);

    // ④ Unwrap 层数一致
    int lower_depth = 0;
    for (auto cursor = lower_wrapped; cursor; cursor = errors::Unwrap(cursor)) {
        ++lower_depth;
    }
    int camel_depth = 0;
    for (auto cursor = camel_wrapped; cursor; cursor = Unwrap(cursor)) {
        ++camel_depth;
    }

    const bool ok = new_equivalent && wrap_equivalent && cross_1 && cross_2 && cross_3 &&
                    lower_depth == 3 && camel_depth == 3;
    std::cout << "  New 等价=" << new_equivalent << " Wrap 等价=" << wrap_equivalent
              << " 交叉判定一致=" << (cross_1 && cross_2 && cross_3)
              << " 链深度=" << lower_depth << "/" << camel_depth
              << "（预期 1 1 1 3/3）\n";
    return ok;
}

}  // namespace

/**
 * @brief error 全面版：7 个场景，覆盖创建、包装、识别、聚合与薄封装等价性。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_create() && ok;
    ok = scenario_wrap_and_unwrap() && ok;
    ok = scenario_is_through_chain() && ok;
    ok = scenario_nil_semantics() && ok;
    ok = scenario_join() && ok;
    ok = scenario_as_by_type() && ok;
    ok = scenario_thin_wrapper_equivalence() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}