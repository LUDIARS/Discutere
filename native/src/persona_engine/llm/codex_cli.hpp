#pragma once
// CodexCliClient (src/persona-engine/llm/codex-cli.ts) — `codex exec` (非対話) を 1 発話ごとに起動する LLMClient。
// - prompt は標準入力で渡す。最終メッセージは `-o <file>` に書かせて読む (上限 1 MiB)。
// - sandbox は read-only、`--ephemeral`。起動は di::lib::one_shot::run (引数配列・シェル不使用・API キー系 env 遮断)。
// - サービス資格情報を子プロセスへ継承しない (環境変数は許可リストだけ渡す)。
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "persona_engine/llm/client.hpp"

namespace di::persona_engine {

struct CodexCliClientOptions {
    /** codex CLI のパス (PATH に乗っていれば不要)。 */
    std::optional<std::string> cliPath;
    /** 既定 model spec (`gpt-5.6-sol@medium` 等)。invoke の model が優先。 */
    std::optional<std::string> defaultModel;
    /** タイムアウト ms (既定 240000)。 */
    std::optional<long long> defaultTimeoutMs;
    /** codex を起動する cwd (既定は現在のディレクトリ)。 */
    std::optional<std::string> cwd;
};

inline constexpr long long CODEX_DEFAULT_TIMEOUT_MS = 240'000;
inline constexpr std::size_t CODEX_MAX_RESPONSE_BYTES = 1'048'576;

/** @implements SPEC-FLOW-MODEL-ROSTER — 許可リストの環境変数だけを残す。 */
std::map<std::string, std::string> build_codex_environment(const std::map<std::string, std::string>& source);
/** codex exec の引数 (codex 以降)。テスト用に公開。 */
std::vector<std::string> codex_cli_args(const std::string& model, const std::optional<std::string>& effort,
                                        const std::string& out_file);

class CodexCliClient : public LLMClient {
public:
    explicit CodexCliClient(CodexCliClientOptions options = {});
    LLMResult invoke(const LLMInvokeArgs& args) override;

private:
    std::string cli_path_;
    std::optional<std::string> default_model_;
    long long default_timeout_ms_;
    std::optional<std::string> cwd_;
};

} // namespace di::persona_engine
