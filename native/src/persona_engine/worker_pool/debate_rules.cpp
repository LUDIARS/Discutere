#include "persona_engine/worker_pool/debate_rules.hpp"

#include "base/strings.hpp"

namespace di::persona_engine::worker_pool {

namespace {
std::string SPEAK(const std::string& role) {
    return "直近の議論を読み、" + role + "として短く一言 (1〜3 文) 発言してください。" +
           "既出の繰り返しや、いま付け足す事が無ければ何も書かず skip してください (発言テキストを空にする)。";
}

RuleSeed seed(std::string id, std::string description, std::string trigger_type, std::optional<std::int64_t> tick_sec,
              std::optional<std::string> event_kind, std::string target, std::int64_t cooldown_sec, std::string instructions) {
    RuleSeed r;
    r.id = std::move(id);
    r.description = std::move(description);
    r.trigger_type = std::move(trigger_type);
    r.tick_sec = tick_sec;
    r.event_kind = std::move(event_kind);
    r.target = std::move(target);
    r.cooldown_sec = cooldown_sec;
    r.instructions = std::move(instructions);
    return r;
}
} // namespace

const std::vector<RuleSeed>& DEBATE_RULE_SEEDS() {
    static const std::vector<RuleSeed> seeds = {
        // ── 人間発言への応答 (最優先・短 cooldown) ──
        seed("human-facilitator", "人間の発言にファシリテーターが即応する", "event", std::nullopt, "HumanUtterance",
             "facilitator", 15,
             std::string("人間の発言を最優先で受け止め、論点を整理して次の一手を一言で投げてください。") + "不要なら skip。"),
        seed("human-opinion-opus", "人間の発言に意見屋(Opus)が反応する", "event", std::nullopt, "HumanUtterance",
             "opinion-opus", 45, SPEAK("意見屋")),
        // ── 自走 debate (tick・役割ごとに cooldown をずらして輪番) ──
        // tick_sec が無いと engine の tick loop で永久に skip され議論が始まらない。cooldown_sec と同値にする。
        seed("tick-con-opus", "否定派(Opus)が反論する", "tick", 80, std::nullopt, "con-opus", 80, SPEAK("否定派")),
        seed("tick-pro-opus", "正論派(Opus)が擁護する", "tick", 85, std::nullopt, "pro-opus", 85, SPEAK("正論派")),
        seed("tick-con-gpt", "否定派(GPT)が反論する", "tick", 110, std::nullopt, "con-gpt", 110, SPEAK("否定派")),
        seed("tick-pro-gpt", "正論派(GPT)が擁護する", "tick", 115, std::nullopt, "pro-gpt", 115, SPEAK("正論派")),
        seed("tick-opinion-sonnet", "意見屋(Sonnet)が独自の角度で", "tick", 100, std::nullopt, "opinion-sonnet", 100,
             SPEAK("意見屋")),
        seed("tick-opinion-gpt", "意見屋(GPT)が独自の角度で", "tick", 125, std::nullopt, "opinion-gpt", 125, SPEAK("意見屋")),
        seed("tick-facilitator-steer", "ファシリテーターが論点を絞る/前提のずれを正して収束を促す", "tick", 150, std::nullopt,
             "facilitator", 150,
             std::string("議論の流れを見て、主張の範囲外から曲げる発言があれば論点に戻し、") +
                 "作りたい体験 / 試したい施策 / 他ゲームの事例のどれがまだ揃っていないかを一つ投げるか、" +
                 "十分まとまっていれば収束を促してください。促しすぎず、不要なら skip。"),
    };
    return seeds;
}

const std::map<std::string, std::string>& RULE_INSTRUCTION_DEFAULTS() {
    static const std::map<std::string, std::string> defaults = [] {
        std::map<std::string, std::string> m;
        for (const auto& r : DEBATE_RULE_SEEDS()) m[r.id] = r.instructions;
        return m;
    }();
    return defaults;
}

std::vector<RuleSeed> apply_rule_instruction_overrides(
    const std::vector<RuleSeed>& seeds, const std::function<std::optional<std::string>(const std::string&)>& get_instruction) {
    std::vector<RuleSeed> out;
    out.reserve(seeds.size());
    for (const auto& r : seeds) {
        const auto ov = get_instruction ? get_instruction(r.id) : std::nullopt;
        if (ov && str::trim(*ov) != "") {
            RuleSeed copy = r;
            copy.instructions = *ov;
            out.push_back(std::move(copy));
        } else {
            out.push_back(r);
        }
    }
    return out;
}

} // namespace di::persona_engine::worker_pool
