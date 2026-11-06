#include "discussion/director.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "base/strings.hpp"

namespace di::discussion {

namespace {
std::string role_label(const PartyMember& m) {
    return m.tier == "facilitator" ? "司会" : m.tier == "keyman" ? "キーマン" : "意見";
}

std::vector<DiscussionUtterance> tail(const std::vector<DiscussionUtterance>& v, std::size_t n) {
    return std::vector<DiscussionUtterance>(v.begin() + static_cast<std::ptrdiff_t>(v.size() > n ? v.size() - n : 0), v.end());
}

void say(const DirectorDeps& deps, const std::string& msg) {
    if (deps.log) deps.log(msg);
}
} // namespace

TurnPrompt build_turn_prompt(const std::string& topic, const PartyMember& m, const std::string& stance,
                             const std::vector<DiscussionUtterance>& recent) {
    const std::string stance_line =
        m.tier == "facilitator"
            ? "あなたは司会。主張・データ・論拠・限定を分けて対立を整理し、作りたい体験 / 試したい施策 / 他ゲームの事例は共通見解がある前提で、合意・止揚・論破のどれかに向けて論点を一つに絞り、必要なら人間に問いかけて巻き込む。"
        : stance == "pro" ? "このターンのあなたの立場は【賛成寄り】。主張を筋の通った形で擁護・補強する。"
                          : "このターンのあなたの立場は【否定寄り】。健全な反論・反例・見落とされた弱点を投げる。";
    TurnPrompt p;
    p.system = str::join(
        {
            "あなたは議論ペルソナ「" + m.personaName + "」(" + role_label(m) + ")。",
            "特徴: " + str::join(m.traits, " / ") + " / 話し方: " + m.speechStyle,
            stance_line,
            "Discord のチャットで実在の人間が話すように、自然な口語で 1〜2 文だけ書く。",
            "ラベルや箇条書きは使わない。既出の繰り返しは避け、議論を一歩進める。",
            "直近の発言と同じ結論・同じ理由の言い換えは禁止。重複しそうなら、未検討のユーザ層、メカニクス条件、反例、判断基準のいずれかを新しく出す。",
            "合意できない場合も同じ賛否を往復せず、未決条件、検証方法、または小さな修正案に分解する。",
            "発言テキストのみを返す (JSON や前置きは不要)。付け足す事が無ければ空行のみ返す。",
        },
        "\n");
    std::string history = "(まだ発言なし)";
    if (!recent.empty()) {
        std::vector<std::string> lines;
        for (const auto& u : recent) lines.push_back(u.personaName + ": " + u.text);
        history = str::join(lines, "\n");
    }
    p.prompt = str::join({"# 議題\n" + topic, "", "# 直近の発言\n" + history, "", "あなたの発言:"}, "\n");
    return p;
}

const PartyMember& pick_speaker(const std::vector<PartyMember>& party, int turn_in_round) {
    const PartyMember* facilitator = nullptr;
    std::vector<const PartyMember*> speakers;
    for (const auto& m : party) {
        if (m.tier == "facilitator") {
            if (!facilitator) facilitator = &m;
        } else {
            speakers.push_back(&m);
        }
    }
    if (turn_in_round > 0 && turn_in_round % 4 == 3 && facilitator) return *facilitator;
    if (speakers.empty()) {
        if (facilitator) return *facilitator;
        throw std::invalid_argument("pick_speaker: empty party");
    }
    return *speakers[static_cast<std::size_t>(turn_in_round) % speakers.size()];
}

DiscussionResult run_discussion(const DiscussionSpec& spec, const DirectorDeps& deps) {
    if (!deps.llm || !deps.rng || !deps.askContinue) throw std::invalid_argument("run_discussion: missing dependency");
    const int max_rounds = deps.maxRounds.value_or(5);
    const double budget = spec.partyConfig.expectedUtterances;
    auto party = form_party(spec.partyConfig, deps.rng);
    {
        std::vector<std::string> parts;
        for (const auto& m : party) parts.push_back(m.personaName + "(" + role_label(m) + "/" + m.model + ")");
        say(deps, "party formed: " + str::join(parts, ", "));
    }

    DiscussionResult result;
    result.stoppedBy = "max-rounds";
    int round = 0;
    while (round < max_rounds) {
        int spoken = 0;
        int turn_in_round = 0;
        int consecutive_skips = 0;
        while (spoken < budget) {
            const PartyMember m = pick_speaker(party, turn_in_round);
            ++turn_in_round;
            const std::string stance = decide_stance(m, deps.rng);
            const auto tp = build_turn_prompt(spec.topic, m, stance, tail(result.utterances, 10));
            const auto res = deps.llm->invoke({tp.system, tp.prompt, m.model});
            std::string text;
            if (!res.ok) {
                say(deps, "turn skip (llm error: " + res.error + ")");
            } else {
                text = str::trim(res.text);
                if (text.empty()) say(deps, "turn skip (" + m.personaName + ": 空応答)");
            }
            if (text.empty()) {
                // TS は失敗し続けると永久に回る。連続失敗がこの数に達したらラウンドを打ち切る。
                if (++consecutive_skips >= kMaxConsecutiveSkips) {
                    say(deps, "round aborted (" + std::to_string(consecutive_skips) + " consecutive skips)");
                    break;
                }
                continue;
            }
            consecutive_skips = 0;
            result.utterances.push_back({m.slotId, m.personaName, text});
            if (deps.postUtterance) deps.postUtterance(m, text);
            ++spoken;
            if (deps.turnDelayMs && *deps.turnDelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(*deps.turnDelayMs));
        }
        // 予算到達 → 続行 / 停止
        if (!deps.askContinue({round + 1, static_cast<int>(result.utterances.size())})) {
            result.stoppedBy = "user-stop";
            break;
        }
        party = swap_half(party, spec.partyConfig, deps.rng);
        std::vector<std::string> names;
        for (const auto& m : party) names.push_back(m.personaName);
        say(deps, "round " + std::to_string(round + 1) + " → swap: " + str::join(names, ", "));
        ++round;
    }

    // 司会のまとめ
    if (deps.postSummary) {
        const PartyMember* facilitator = nullptr;
        for (const auto& m : party)
            if (m.tier == "facilitator") {
                facilitator = &m;
                break;
            }
        if (facilitator) {
            const auto tp = build_turn_prompt(
                spec.topic + "\n(これは議論の締め。ここまでの論点を 2〜3 文で要約し、到達した結論または残った論点を述べる)",
                *facilitator, "neutral", tail(result.utterances, 12));
            const auto res = deps.llm->invoke({tp.system, tp.prompt, facilitator->model});
            const std::string text = res.ok ? str::trim(res.text) : std::string();
            if (!text.empty()) deps.postSummary(*facilitator, text);
        }
    }
    result.rounds = round + 1;
    return result;
}

} // namespace di::discussion
