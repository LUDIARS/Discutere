#pragma once
// 最小のテスト枠。各 *_tests.cpp は main() で di::test::run_all() を呼ぶ。
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace di::test {

struct Case {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

class Failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void check(bool condition, std::string_view what, const char* file, int line) {
    if (!condition)
        throw Failure(std::string(file) + ":" + std::to_string(line) + ": check failed: " + std::string(what));
}

template <class A, class B>
void check_eq(const A& actual, const B& expected, std::string_view what, const char* file, int line) {
    if (!(actual == expected)) {
        std::string message = std::string(file) + ":" + std::to_string(line) + ": " + std::string(what);
        if constexpr (requires(std::ostream& os) { os << actual; os << expected; }) {
            std::ostringstream detail;
            detail << "\n  actual:   " << actual << "\n  expected: " << expected;
            message += detail.str();
        }
        throw Failure(message);
    }
}

template <class Fn>
void check_throws(Fn&& fn, std::string_view what, const char* file, int line) {
    try {
        fn();
    } catch (...) {
        return;
    }
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": expected exception: " + std::string(what));
}

/** fixtures ディレクトリ (ctest が DI_TEST_FIXTURES を渡す)。 */
inline std::string fixtures_dir() {
    const char* value = std::getenv("DI_TEST_FIXTURES");
    return value ? std::string(value) : std::string("native/tests/fixtures");
}

inline int run_all() {
    int failed = 0;
    for (const auto& c : registry()) {
        try {
            c.body();
            std::cout << "ok   " << c.name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << c.name << "\n  " << e.what() << '\n';
        }
    }
    std::cout << (registry().size() - static_cast<std::size_t>(failed)) << "/" << registry().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace di::test

#define DI_TEST_CAT2(a, b) a##b
#define DI_TEST_CAT(a, b) DI_TEST_CAT2(a, b)
#define TEST_CASE(name) \
    static void DI_TEST_CAT(di_test_fn_, __LINE__)(); \
    static ::di::test::Register DI_TEST_CAT(di_test_reg_, __LINE__)(name, &DI_TEST_CAT(di_test_fn_, __LINE__)); \
    static void DI_TEST_CAT(di_test_fn_, __LINE__)()
#define CHECK(cond) ::di::test::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::di::test::check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_THROWS(expr) ::di::test::check_throws([&] { (void)(expr); }, #expr, __FILE__, __LINE__)
#define DI_TEST_MAIN() int main() { return ::di::test::run_all(); }
