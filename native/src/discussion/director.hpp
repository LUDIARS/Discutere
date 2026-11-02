#pragma once
// DiscussionDirector — 1 議論の進行オーケストレータ (src/discussion/director.ts)。
// パーティ編成 → ターン進行 → 予算到達で続行/停止を問う → 続行なら半数入替して次ラウンド、
// 停止なら司会がまとめて終了。LLM 呼び出し / 投稿 / 続行の問いは依存注入 (Discord 等の実体は後続の担当が結線する)。
// 同期 API: 呼んだスレッド (worker) で進行を待つ。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "discussion/composition.hpp"

namespace di::discussion {

struct DirectorLLMArgs {
    std::string system;
    std::string prompt;
    std::string model;
};

/** { ok: true, text } | { ok: false, error } */
struct DirectorLLMResult {
    bool ok = false;
    std::string text;
    std::string error;
};

class DirectorLLM {
public:
    virtual ~DirectorLLM() = default;
    virtual DirectorLLMResult invoke(const DirectorLLMArgs& args) = 0;
};

struct ContinueContext {
    int round = 0;
    int total = 0;
};

struct DirectorDeps {
    DirectorLLM* llm = nullptr;
    /** 発話を出力先 (Discord 等) に流す。 */
    std::function<void(const PartyMember&, const std::string&)> postUtterance;
    /** 予算到達時に「続ける / 止める」を問う。true=続ける (無応答タイムアウトは false)。 */
    std::function<bool(const ContinueContext&)> askContinue;
    /** 終了時の司会まとめを流す (任意)。 */
    std::function<void(const PartyMember&, const std::string&)> postSummary;
    std::function<void(const std::string&)> log;
    Rng rng;
    /** ターン間ディレイ ms (既定 0)。 */
    std::optional<long long> turnDelayMs;
    /** ラウンド上限 (暴走ガード、既定 5)。 */
    std::optional<int> maxRounds;
};

struct DiscussionSpec {
    std::string topic;
    PartyConfig partyConfig;
};

struct DiscussionUtterance {
    std::string slotId;
    std::string personaName;
    std::string text;
};

struct DiscussionResult {
    std::vector<DiscussionUtterance> utterances;
    int rounds = 0;
    /** "user-stop" | "max-rounds" */
    std::string stoppedBy;
};

struct TurnPrompt {
    std::string system;
    std::string prompt;
};

/** ターンのプロンプト (テスト / golden 用に公開)。 */
TurnPrompt build_turn_prompt(const std::string& topic, const PartyMember& m, const std::string& stance,
                             const std::vector<DiscussionUtterance>& recent);
/** 次に喋るメンバー。意見/キーマンを巡回し、4 ターンに 1 回 (冒頭除く) 司会を挟む。 */
const PartyMember& pick_speaker(const std::vector<PartyMember>& party, int turn_in_round);

/** LLM が失敗し続けたとき 1 ラウンドを打ち切る連続失敗数 (TS には無い暴走ガード)。 */
inline constexpr int kMaxConsecutiveSkips = 20;

DiscussionResult run_discussion(const DiscussionSpec& spec, const DirectorDeps& deps);

} // namespace di::discussion
