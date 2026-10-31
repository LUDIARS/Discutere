#include "check.hpp"

#include "base/process.hpp"

using namespace di;

#ifndef _WIN32
TEST_CASE("argv is passed without a shell") {
    proc::Options o;
    o.argv = {"printf", "%s|", "a b", "$(echo injected)", ";rm -rf /"};
    auto r = proc::run(o);
    CHECK(r.started);
    CHECK_EQ(r.exit_code, 0);
    CHECK_EQ(r.out, std::string("a b|$(echo injected)|;rm -rf /|"));
}

TEST_CASE("stdin is written and large output is read") {
    proc::Options o;
    o.argv = {"cat"};
    o.stdin_data = std::string(1 << 20, 'x');
    auto r = proc::run(o);
    CHECK_EQ(r.out.size(), std::size_t(1 << 20));
}

TEST_CASE("timeout kills the process") {
    proc::Options o;
    o.argv = {"sleep", "10"};
    o.timeout = std::chrono::milliseconds(300);
    auto r = proc::run(o);
    CHECK(r.timed_out);
    CHECK(r.duration_ms < 5000);
}

TEST_CASE("output over the limit stops the process (boundary)") {
    proc::Options o;
    o.argv = {"head", "-c", "1000", "/dev/zero"};
    o.max_stdout = 1000;
    auto ok = proc::run(o);
    CHECK(!ok.output_exceeded);
    CHECK_EQ(ok.out.size(), std::size_t(1000));
    o.max_stdout = 999;
    auto over = proc::run(o);
    CHECK(over.output_exceeded);
}

TEST_CASE("missing executable is reported, not run through a shell") {
    proc::Options o;
    o.argv = {"definitely-not-a-command-xyz"};
    auto r = proc::run(o);
    CHECK(!r.started);
}

TEST_CASE("env is replaced when given") {
    proc::Options o;
    o.argv = {"/usr/bin/env"};
    o.env = std::map<std::string, std::string>{{"ONLY", "1"}};
    auto r = proc::run(o);
    CHECK_EQ(r.out, std::string("ONLY=1\n"));
}
#endif

TEST_CASE("windows argument quoting follows MSVCRT rules") {
    CHECK_EQ(proc::quote_windows_arg("abc"), std::string("abc"));
    CHECK_EQ(proc::quote_windows_arg("a b"), std::string("\"a b\""));
    CHECK_EQ(proc::quote_windows_arg("a\"b"), std::string("\"a\\\"b\""));
    CHECK_EQ(proc::quote_windows_arg("a\\ b\\"), std::string("\"a\\ b\\\\\""));
    CHECK_EQ(proc::quote_windows_arg(""), std::string("\"\""));
}

DI_TEST_MAIN()
