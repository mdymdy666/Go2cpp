#include "go2cpp/panic_defer.hpp"
#include "test_support.hpp"

#include <string>
#include <stdexcept>
#include <vector>

void run_panic_defer_tests() {
    go2cpp_tests::announce("defer, panic and recover");
    using namespace go2cpp::panic_defer;

    std::vector<int> order;
    const bool completed = run([&] {
        Frame frame;
        frame.defer_call([&] { order.push_back(1); });
        frame.defer_call([&] { order.push_back(2); });
        frame.defer_call([&] { order.push_back(3); });
        int captured = 10;
        frame.defer_call([&](int value) { order.push_back(value); }, captured);
        captured = 99;
        frame.finish();
    });
    GO2CPP_CHECK(completed);
    GO2CPP_CHECK((order == std::vector<int>{10, 3, 2, 1}));

    std::vector<int> nested_order;
    GO2CPP_CHECK(run([&] {
        Frame frame;
        frame.defer_call([&] {
            nested_order.push_back(1);
            frame.defer_call([&] { nested_order.push_back(2); });
        });
        frame.finish();
    }));
    GO2CPP_CHECK((nested_order == std::vector<int>{1, 2}));

    std::string nested_frame_recovery;
    GO2CPP_CHECK(run([&] {
        Frame frame;
        frame.defer_call([&] {
            {
                Frame nested;
                nested.finish();
            }
            const auto value = recover();
            if (const auto* text = value.as_text()) {
                nested_frame_recovery = *text;
            }
        });
        panic(PanicValue::text("nested frame"));
    }));
    GO2CPP_CHECK(nested_frame_recovery == "nested frame");

    std::string recovered;
    const bool recovered_run = run([&] {
        Frame frame;
        frame.defer_call([&] {
            const auto value = recover();
            if (const auto* text = value.as_text()) {
                recovered = *text;
            }
        });
        panic(PanicValue::text("boom"));
    });
    GO2CPP_CHECK(recovered_run);
    GO2CPP_CHECK(recovered == "boom");
    GO2CPP_CHECK(!panicking());

    bool unhandled_called = false;
    set_unhandled_panic_handler([&](const PanicValue& value) {
        unhandled_called = value.nil_payload();
    });
    const bool nil_completed = run([&] {
        Frame frame;
        frame.defer_call([] {});
        panic_nil();
    });
    GO2CPP_CHECK(!nil_completed);
    GO2CPP_CHECK(unhandled_called);
    set_unhandled_panic_handler({});

    bool translated_exception = false;
    set_unhandled_panic_handler([&](const PanicValue& value) {
        translated_exception = value.as_text() != nullptr;
    });
    const bool exception_completed = run([] {
        throw std::runtime_error("body failure");
    });
    GO2CPP_CHECK(!exception_completed);
    GO2CPP_CHECK(translated_exception);
    set_unhandled_panic_handler({});

    bool frame_recovered_throw = false;
    GO2CPP_CHECK(!run([&] {
        Frame frame;
        frame.defer_call([&] { frame_recovered_throw = recover().valid(); });
        throw std::runtime_error("throw during frame unwinding");
    }));
    GO2CPP_CHECK(!frame_recovered_throw);

    bool throwing_handler_called = false;
    set_unhandled_panic_handler([&](const PanicValue&) {
        throwing_handler_called = true;
        throw std::runtime_error("handler failure");
    });
    GO2CPP_CHECK(!run([] { panic(PanicValue::text("handler boundary")); }));
    GO2CPP_CHECK(throwing_handler_called);
    set_unhandled_panic_handler({});

    std::string defer_panic_text;
    const bool defer_recovered = run([&] {
        Frame frame;
        frame.defer_call([&] {
            const auto value = recover();
            if (const auto* text = value.as_text()) {
                defer_panic_text = *text;
            }
        });
        frame.defer_call([] { panic(PanicValue::text("defer panic")); });
        frame.finish();
    });
    GO2CPP_CHECK(defer_recovered);
    GO2CPP_CHECK(defer_panic_text == "defer panic");

    PanicValue outside;
    run([&] { outside = recover(); });
    GO2CPP_CHECK(!outside.valid());
    PanicValue other_scope;
    run([&] {
        panic(PanicValue::text("isolated"));
        run([&] { other_scope = recover(); });
    });
    GO2CPP_CHECK(!other_scope.valid());
}
