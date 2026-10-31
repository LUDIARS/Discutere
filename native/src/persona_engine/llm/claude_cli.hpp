#pragma once
// src/persona-engine/llm/claude-cli.ts — `claude -p --output-format json` を引数配列で起動する LLMClient。
// プロンプトは標準入力で渡す。期限の既定は 120 秒 (claude -p は 1 回 8〜11 秒)。API キーは持たない。
#include <optional>
#include <vector>
#include <string>

#include "persona_engine/llm/client.hpp"

namespace di::persona_engine {

struct ClaudeCliClientOptions {
    std::optional<std::string> cliPath;        // 既定 "claude" (PATH から探す)
    std::optional<std::string> defaultModel;
    std::optional<long long> defaultTimeoutMs; // 既定 120000
    std::optional<std::string> gitBashPath;    // Windows 用
};

class ClaudeCliClient : public LLMClient {
public:
    explicit ClaudeCliClient(ClaudeCliClientOptions options = {});
    LLMResult invoke(const LLMInvokeArgs& args) override;

private:
    std::string cli_path_;
    long long default_timeout_ms_;
    std::optional<std::string> git_bash_path_;
    std::optional<std::string> default_model_;
};

/** claude -p --output-format json の stdout を読む (純関数)。 */
LLMResult parse_claude_cli_result(const std::string& raw);
/** system があれば "[system]\n...\n\n[user]\n..." にする。 */
std::string compose_prompt(const std::optional<std::string>& system, const std::string& user);
/** 起動する引数 (claude 以降)。テスト用に公開。 */
std::vector<std::string> claude_cli_args(const std::optional<std::string>& model, bool conversation_only);

} // namespace di::persona_engine
