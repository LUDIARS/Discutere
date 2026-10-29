// tests/persona-engine の claude-cli 部分 (parseClaudeCliResult / 起動引数) の移植。
#include "check.hpp"

#include "persona_engine/llm/claude_cli.hpp"
#include "persona_engine/llm/model_spec.hpp"
#include "lib/one_shot/one_shot.hpp"
#include "base/fsutil.hpp"
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace di::persona_engine;

TEST_CASE("parses single result envelope") {
    auto r = parse_claude_cli_result(R"({"type":"result","subtype":"success","is_error":false,"result":" hi ","total_cost_usd":0.01,"usage":{"input_tokens":3,"output_tokens":4}})");
    CHECK(r.ok);
    CHECK_EQ(r.text, std::string("hi"));
    CHECK_EQ(*r.usage->input_tokens, 3.0);
    CHECK_EQ(*r.usage->cost_usd, 0.01);
}

TEST_CASE("parses event array") {
    auto r = parse_claude_cli_result(R"([{"type":"system"},{"type":"result","subtype":"success","result":"ok"}])");
    CHECK(r.ok);
    CHECK_EQ(r.text, std::string("ok"));
}

TEST_CASE("error envelopes and empty text fail") {
    CHECK(!parse_claude_cli_result(R"({"type":"result","subtype":"error_max_turns","result":"x"})").ok);
    CHECK(!parse_claude_cli_result(R"({"type":"result","is_error":true,"result":"x"})").ok);
    CHECK(!parse_claude_cli_result(R"({"type":"result","result":"   "})").ok);
    CHECK(!parse_claude_cli_result("not json").ok);
}

TEST_CASE("cli args carry model and effort as separate argv entries") {
    auto a = claude_cli_args(std::string("claude-opus-5@xhigh"), true);
    std::vector<std::string> expected = {"-p", "--output-format", "json", "--tools=", "--strict-mcp-config",
                                         "--disable-slash-commands", "--safe-mode", "--model", "claude-opus-5", "--effort", "xhigh"};
    CHECK(a == expected);
    CHECK(validate_model_spec(parse_model_spec(std::string("bad model;rm")), ModelProvider::claude).has_value());
    CHECK(!validate_model_spec(parse_model_spec(std::string("claude-x@mid")), ModelProvider::claude).has_value());
    CHECK(validate_model_spec(parse_model_spec(std::string("claude-x@minimal")), ModelProvider::claude).has_value());
    CHECK(is_codex_model(std::string("GPT-6-sol@low")));
}

TEST_CASE("one-shot strips API keys and resolves default model") {
    std::map<std::string, std::string> env = {{"ANTHROPIC_API_KEY", "x"}, {"LICTOR_PORT", "1"}, {"PATH", "/usr/bin"}, {"HOME", "/h"}};
    auto launch = di::lib::one_shot::prepare("claude", {"-p", "--output-format", "json"}, env, "/tmp");
    CHECK(launch.env.find("ANTHROPIC_API_KEY") == launch.env.end());
    CHECK(launch.env.find("LICTOR_PORT") == launch.env.end());
    CHECK_EQ(launch.env.at("HOME"), std::string("/h"));
    CHECK_EQ(launch.args[0], std::string("--model"));
    CHECK_EQ(launch.args[1], std::string("claude-sonnet-5"));
    CHECK_THROWS(di::lib::one_shot::prepare("claude", {"--help"}, env, "/tmp"));
    CHECK_THROWS(di::lib::one_shot::prepare("cmd.exe", {"/c", "claude -p"}, env, "/tmp"));
    auto codex = di::lib::one_shot::prepare("codex", {"exec", "--model", "sol", "-"}, env, "/tmp");
    CHECK_EQ(codex.args[2], std::string("gpt-6-sol"));
}

#ifndef _WIN32
TEST_CASE("invoke runs a fake claude through argv and stdin") {
    // 偽の claude: 標準入力をそのまま result に入れて返す (本物の LLM には接続しない)。
    const std::string dir = "/tmp/di-fake-claude-" + std::to_string(::getpid());
    di::fs::ensure_dir(dir);
    const std::string fake = dir + "/claude";
    di::fs::write_file_atomic(fake, "#!/bin/sh\ninput=$(cat)\nprintf '{\"type\":\"result\",\"subtype\":\"success\",\"result\":\"%s\"}' \"$(printf %s \"$input\" | tr -d '\\n\\\"')\"\n");
    ::chmod(fake.c_str(), 0755);
    ClaudeCliClient client({fake, std::nullopt, 10'000, std::nullopt});
    LLMInvokeArgs args;
    args.prompt = "hello";
    args.system = "sys";
    auto r = client.invoke(args);
    CHECK(r.ok);
    CHECK_EQ(r.text, std::string("[system]sys[user]hello"));
}
#endif

DI_TEST_MAIN()
