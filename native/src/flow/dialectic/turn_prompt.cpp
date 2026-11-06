#include "flow/dialectic/turn_prompt.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "flow/discussion_paper.hpp"

namespace di::flow::dialectic {

std::string render_position_digest(const PositionRecord& p, const std::string& persona_name) {
    std::vector<std::string> grounds;
    for (const auto& g : p.grounds) {
        const std::string kind = g.kind ? " " + inference_kind_label(*g.kind) : "";
        const std::string warrant = (g.warrant && !g.warrant->empty()) ? "\n      論拠: " + *g.warrant : "";
        grounds.push_back("  - [" + g.id + "] (" + g.state + kind + ") データ: " + g.text + warrant);
    }
    const std::string qualifier = (p.qualifier && !p.qualifier->empty()) ? "\n  限定: " + *p.qualifier : "";
    const std::string values = p.values.empty() ? "" : "\n  価値前提: " + str::join(p.values, " / ");
    return persona_name + " (" + p.stance + "): " + p.claim + qualifier + "\n" + str::join(grounds, "\n") + values;
}

std::string build_dialectic_turn_prompt(const DialecticTurnPromptArgs& args) {
    const auto& persona = args.persona;
    auto name_of = [&](const std::string& id) {
        auto it = args.personaNames.find(id);
        return it == args.personaNames.end() ? id : it->second;
    };
    std::vector<std::string> position_blocks;
    for (const auto& p : args.positions) {
        const std::string owner = name_of(p.personaId);
        position_blocks.push_back(render_position_digest(p, p.personaId == persona.id ? owner + " (あなた)" : owner));
    }
    const std::string position_block = str::join(position_blocks, "\n\n");

    std::string unresolved_block = "(未応答の反論なし)";
    if (!args.unresolvedRebuttals.empty()) {
        std::vector<std::string> lines;
        for (const auto& e : args.unresolvedRebuttals) {
            auto rebut = args.utterances.find(e.rebutId);
            auto target = args.utterances.find(e.targetId);
            lines.push_back("- [" + e.rebutId + "] " +
                            (rebut != args.utterances.end()
                                 ? rebut->second.personaName + ": " + str::js_slice(rebut->second.text, 0, 120)
                                 : std::string("(反論)")) +
                            (target != args.utterances.end() ? " → 対象: " + target->second.personaName + " の主張" : ""));
        }
        unresolved_block = str::join(lines, "\n");
    }

    std::string target_block;
    if (args.targetUtteranceId && !args.targetUtteranceId->empty()) {
        auto t = args.utterances.find(*args.targetUtteranceId);
        if (t != args.utterances.end())
            target_block = "# 応答対象\n[" + t->second.id + "] " + t->second.personaName + ": " + t->second.text +
                           "\nこの発言に応答してください。\n";
    }
    const std::string nomination_line =
        (args.nomination && !args.nomination->empty()) ? "進行役: " + *args.nomination + "\n" : "";
    const std::string act_list = str::join(args.allowedActs, " | ");

    std::vector<std::string> lines{"あなたは議論ペルソナ「" + persona.name + "」。",
                                   "特徴: " + str::join(persona.traits, " / ") + " / 話し方: " + persona.speechStyle};
    if (persona.valueAxis && !persona.valueAxis->empty()) lines.push_back("あなたが重視する価値: " + *persona.valueAxis);
    lines.push_back(stance_line(args.stance));
    lines.emplace_back("");
    lines.push_back(premise_rules_text());
    lines.emplace_back("");
    lines.push_back("# 論点 " + std::to_string(args.issue.ordinal));
    lines.push_back(args.issue.title);
    lines.emplace_back("");
    if (args.voicesBlock && !args.voicesBlock->empty()) {
        lines.push_back(*args.voicesBlock);
        lines.emplace_back("");
    }
    lines.emplace_back("# 現在の論証状態 (根拠は [id] で指す)");
    lines.push_back(position_block.empty() ? "(Position なし)" : position_block);
    lines.emplace_back("");
    lines.emplace_back("# 未応答の反論");
    lines.push_back(unresolved_block);
    lines.emplace_back("");
    lines.push_back(
        nomination_line + target_block + "# 指示\n" +
        "あなたの発言は次の JSON 1 個だけで返す (前後に説明やコードフェンスを付けない):\n" + "{\"act\": \"<" + act_list +
        ">\", \"target\": \"<応答先 utterance id (claim/question は null 可)>\", " +
        "\"attack\": \"<rebut のとき必須: data | warrant | qualifier (突くのはデータ・論拠・限定のどれか)>\", " +
        "\"groundId\": \"<rebut で data/warrant を突くとき必須: 攻撃対象の根拠 id>\", " +
        "\"data\": \"<rebut のとき必須: 反論のデータ (事実・事例・仕様)>\", " +
        "\"warrant\": \"<rebut のとき必須: 反論の論拠 (そのデータで相手のどこが崩れるか)>\", " +
        "\"text\": \"<Discord に流す口語 1〜3 文>\"}\n" + "act は今の議論状態で最も議論を前に進める手を選ぶ (許可: " + act_list +
        ")。\n" +
        "rebut は相手の主張の限定の内側で、宣言したデータ・論拠・限定を突く。反論もトゥールミンモデルを満たす (反論自身のデータと論拠を書く)。満たさない反論・突く先を示せない反論は根拠への反論として数えない。\n" +
        "text は実在の人間の雑談のような自然な口語で書く (ラベル・箇条書き禁止)。");
    return str::join(lines, "\n");
}

Json to_json(const ParsedTurn& t) {
    auto opt = [](const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); };
    Json o = Json::object();
    o["act"] = t.act;
    o["targetId"] = opt(t.targetId);
    o["groundId"] = opt(t.groundId);
    o["attack"] = opt(t.attack);
    o["data"] = opt(t.data);
    o["warrant"] = opt(t.warrant);
    o["text"] = t.text;
    o["degraded"] = t.degraded;
    return o;
}

namespace {
std::optional<std::string> trimmed(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return std::nullopt;
    auto t = str::trim(it->get<std::string>());
    if (t.empty()) return std::nullopt;
    return t;
}
} // namespace

ParsedTurn parse_turn_response(const std::string& raw, const ParseTurnOptions& opts) {
    const std::string fallback_text = str::trim(raw);
    const auto obj = extract_json_object(raw);
    const auto text = obj ? trimmed(*obj, "text") : std::nullopt;
    if (!obj || !text) {
        if (opts.warn)
            opts.warn("発話 JSON パース失敗 — 全文を act=claim として受理 (degrade): " + str::js_slice(fallback_text, 0, 60));
        return {"claim", std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, fallback_text, true};
    }
    const auto act_it = obj->find("act");
    DialogueAct act = coerce_act(act_it != obj->end() && act_it->is_string() ? std::optional<std::string>(act_it->get<std::string>())
                                                                            : std::nullopt);
    if (std::find(opts.allowedActs.begin(), opts.allowedActs.end(), act) == opts.allowedActs.end()) {
        const std::string forced = opts.allowedActs.empty() ? "claim" : opts.allowedActs[0];
        if (opts.warn)
            opts.warn("act \"" + act + "\" は許可外 (許可: " + str::join(opts.allowedActs, ",") + ") — \"" + forced + "\" に矯正");
        act = forced;
    }
    std::optional<std::string> target_id;
    const auto target_it = obj->find("target");
    const std::string raw_target = (target_it != obj->end() && target_it->is_string()) ? str::trim(target_it->get<std::string>()) : "";
    if (!raw_target.empty() && opts.validTargets.count(raw_target)) {
        target_id = raw_target;
    } else {
        if (!raw_target.empty() && opts.warn)
            opts.warn("target \"" + raw_target + "\" は不明な utterance id — 指名ターゲットに置換");
        target_id = opts.defaultTargetId;
    }
    // 応答系 act なのに target が無い場合も指名ターゲットへフォールバック。
    if ((!target_id || target_id->empty()) && (act == "rebut" || act == "support" || act == "concede"))
        target_id = opts.defaultTargetId;
    ParsedTurn t;
    t.act = act;
    t.targetId = target_id;
    t.groundId = trimmed(*obj, "groundId");
    t.attack = coerce_attack_point(obj->contains("attack") ? (*obj)["attack"] : Json());
    t.data = trimmed(*obj, "data");
    t.warrant = trimmed(*obj, "warrant");
    t.text = *text;
    t.degraded = false;
    return t;
}

} // namespace di::flow::dialectic
