#include "flow/dialectic/synthesis.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "base/strings.hpp"

namespace di::flow::dialectic {

std::string synthesis_template_for(const TensionType& type) {
    if (type == "values")
        return std::string("これは価値対立です。上位価値の発見、または適用条件の分割 (「コア層には X、ライト層には Y」") +
               "のように、どちらの価値も条件付きで生かす分割) で統合してください。";
    if (type == "means")
        return std::string("これは手段対立です (目標は共有)。共通目標を再確認した上で、両者の手段の合成・段階化") +
               " (まず A で始めて条件を満たしたら B へ、など) で統合してください。";
    if (type == "tradeoff")
        return std::string("これはトレードオフです。二者択一に見える 2 変数をパラメータ化し、両方を部分的に満たす") +
               "非線形の第 3 案 (構造を変えることでトレードオフ自体を無効化する案) を出してください。";
    if (type == "facts") return "対立する主張を条件分割で統合してください。";
    return "";
}

namespace {

std::string render_position_for_synthesis(const std::string& label, const PositionRecord& p) {
    std::vector<std::string> grounds;
    for (const auto& g : p.grounds) grounds.push_back("  - [" + g.id + "] (" + g.state + ") " + g.text);
    const std::string values = str::join(p.values, " / ");
    return label + " (" + p.stance + "): " + p.claim + "\n" + str::join(grounds, "\n") + "\n  価値前提: " +
           (values.empty() ? "(なし)" : values);
}

std::vector<std::string> coerce_id_array(const Json& v, const std::set<std::string>& valid) {
    std::vector<std::string> out;
    if (!v.is_array()) return out;
    for (const auto& s : v) {
        if (!s.is_string()) continue;
        auto t = str::trim(s.get<std::string>());
        if (!t.empty() && valid.count(t)) out.push_back(std::move(t));
    }
    return out;
}

const Json& field(const Json& o, const char* key) {
    static const Json kNull;
    if (!o.is_object()) return kNull;
    auto it = o.find(key);
    return it == o.end() ? kNull : *it;
}

bool contains_ci_ascii(const std::string& s, std::string_view needle) {
    return str::contains(str::to_lower_ascii(s), needle);
}

} // namespace

std::string build_synthesis_prompt(const BuildSynthesisPromptArgs& args) {
    std::string feedback;
    if (args.missingFeedback && !args.missingFeedback->empty()) {
        std::vector<std::string> lines;
        for (const auto& m : *args.missingFeedback) lines.push_back("- " + m);
        feedback = "\n# 前回候補への批准フィードバック (これらの根拠が保存されておらず reject された)\n" + str::join(lines, "\n") +
                   "\n前回候補の言い換えは禁止です。欠けていた根拠を、どの条件・対象・検証方法で保存したかが分かる形に修正してください。\n";
    }
    return "# 論点\n" + args.issueTitle + "\n\n" + "# 対立する Position (根拠は [id] で指す)\n" +
           render_position_for_synthesis("A", args.positionA) + "\n\n" + render_position_for_synthesis("B", args.positionB) +
           "\n\n" + "# 指示\n" + synthesis_template_for(args.tension.type) + "\n" + feedback +
           "\n止揚 (aufheben) の 3 要件をデータで示してください:\n" +
           "- preserves: 各陣営から何を保存したか (上の根拠 id で指す。両陣営から挙げる)\n" +
           "- negates: 何を放棄させたか (根拠 id)\n" +
           "- elevates: 導入した新しい枠 (上位目標 / 条件分割 / 再定義)。単なる「両方やる」は不可\n\n" +
           "text には、保存した根拠がどう両立するかを短く含める。既存案の反復や「状況に応じて両方」は不可。\n\n" +
           "次の JSON 1 個だけを返してください (前後に説明やコードフェンスを付けない):\n" +
           "{\"text\": \"<統合案の口語 1〜3 文>\", \"preserves\": [\"<根拠id>\"], \"negates\": [\"<根拠id>\"], " +
           "\"elevates\": \"<新しい枠の説明 1 文>\"}";
}

GeneratedSynthesis generate_synthesis(persona_engine::LLMClient& llm, const GenerateSynthesisArgs& args) {
    std::set<std::string> valid;
    for (const auto& g : args.positionA.grounds) valid.insert(g.id);
    for (const auto& g : args.positionB.grounds) valid.insert(g.id);
    persona_engine::LLMInvokeArgs call;
    call.system = args.paperSystem;
    call.prompt = build_synthesis_prompt(args);
    if (args.model && !args.model->empty()) call.model = *args.model;
    const auto result = llm.invoke(call);
    if (!result.ok) {
        if (args.warn) args.warn("Synthesis 生成 LLM エラー: " + result.error + " — 空候補で degrade (折衷扱い)");
        return {args.positionA.claim + " と " + args.positionB.claim + " の統合案は生成できませんでした", {}, {}, std::nullopt, true};
    }
    const auto obj = extract_json_object(result.text);
    const Json& text = obj ? field(*obj, "text") : field(Json(), "text");
    if (!obj || !text.is_string() || str::trim(text.get<std::string>()).empty()) {
        if (args.warn) args.warn("Synthesis 生成 JSON パース失敗 — 全文を折衷候補として受理 (degrade)");
        return {str::trim(result.text), {}, {}, std::nullopt, true};
    }
    const Json& elev = field(*obj, "elevates");
    std::optional<std::string> elevates;
    if (elev.is_string() && !str::trim(elev.get<std::string>()).empty()) elevates = str::trim(elev.get<std::string>());
    return {str::trim(text.get<std::string>()), coerce_id_array(field(*obj, "preserves"), valid),
            coerce_id_array(field(*obj, "negates"), valid), elevates, false};
}

std::string elevation_gate(persona_engine::LLMClient& llm, const std::optional<std::string>& elevates,
                           const std::optional<std::string>& model, const WarnFn& warn) {
    if (!elevates || str::trim(*elevates).empty()) return "compromise";
    // 「両方やる」系の言い回し (/両方(と|や|を)?/, /どちらも(やる|行う|採用)/, /both/i)。短文はコードで折衷判定。
    const std::string& e = *elevates;
    const bool both = str::contains(e, "両方") || str::contains(e, "どちらもやる") || str::contains(e, "どちらも行う") ||
                      str::contains(e, "どちらも採用") || contains_ci_ascii(e, "both");
    if (both && str::js_length(e) < 20) return "compromise";

    persona_engine::LLMInvokeArgs call;
    call.prompt = "統合案の「新しい枠」の記述です:\n\"" + e + "\"\n\n" +
                  "これは (a) 上位目標の発見・適用条件の分割・問題の再定義など**新しい枠の導入**か、" +
                  "(b) 実質「両方やる」「折半する」でしかない**折衷**か。\n" +
                  "aufheben (新しい枠) / compromise (折衷) のどちらか 1 語のみで答えてください。迷ったら compromise。";
    if (model && !model->empty()) call.model = *model;
    call.maxTokens = 50;
    const auto result = llm.invoke(call);
    if (!result.ok) {
        if (warn) warn("elevation ゲート LLM エラー: " + result.error + " — compromise に倒す (degrade)");
        return "compromise";
    }
    const std::string t = str::to_lower(result.text);
    const auto ia = t.find("aufheben");
    const auto ic = t.find("compromise");
    if (ia != std::string::npos && (ic == std::string::npos || ia < ic)) return "aufheben";
    if (ic != std::string::npos) return "compromise";
    if (warn) warn("elevation ゲート ラベル不明: \"" + str::js_slice(result.text, 0, 60) + "\" — compromise に倒す (degrade)");
    return "compromise";
}

std::string build_ratify_prompt(const BuildRatifyPromptArgs& args) {
    const auto& position = args.position;
    std::vector<std::string> ids;
    std::vector<std::string> list;
    for (const auto& g : position.grounds) {
        ids.push_back(g.id);
        list.push_back("- [" + g.id + "] " + g.text);
    }
    const std::string preserves = str::join(args.preserves, ", ");
    return "あなたは議論ペルソナ「" + args.personaName + "」。この議論でのあなたの立場は " + position.stance + "、" +
           "主張は「" + position.claim + "」です。\n\n" + "# あなたの主張の根拠\n" + str::join(list, "\n") + "\n\n" +
           "# 提示された統合案\n" + args.synthesisText + "\n" +
           ((args.elevates && !args.elevates->empty()) ? "新しい枠: " + *args.elevates + "\n" : "") +
           "(統合案が保存を主張する根拠: [" + (preserves.empty() ? "なし" : preserves) + "])\n\n" +
           "# チェックリスト (敵対的批准 — 甘い受諾をしない)\n" + "あなたの主張の根拠は [" + str::join(ids, ", ") + "] だった。" +
           "この統合案で**実際に保存されているもの**を ID で挙げよ。" +
           "保存が確認できた根拠が過半数に満たなければ reject し、欠けているものを指摘せよ。" +
           "**デフォルトは reject** — 保存を自分で確認できたときだけ accept せよ。\n\n" + "次の JSON 1 個だけを返してください:\n" +
           "{\"verdict\": \"accept\" | \"reject\", \"preservedIds\": [\"<確認できた根拠id>\"], " +
           "\"missing\": [\"<欠けている根拠id>\"], \"reason\": \"<一言>\"}";
}

RatificationEntry ratify_synthesis(persona_engine::LLMClient& llm, const RatifyArgs& args) {
    const auto& position = args.position;
    std::vector<std::string> own_ids;  // Set の挿入順
    for (const auto& g : position.grounds)
        if (std::find(own_ids.begin(), own_ids.end(), g.id) == own_ids.end()) own_ids.push_back(g.id);
    auto is_own = [&](const std::string& id) { return std::find(own_ids.begin(), own_ids.end(), id) != own_ids.end(); };
    auto reject_default = [&](const std::string& reason) { return RatificationEntry{position.id, "reject", {}, own_ids, reason}; };

    persona_engine::LLMInvokeArgs call;
    call.prompt = build_ratify_prompt(args);
    if (args.model && !args.model->empty()) call.model = *args.model;
    const auto result = llm.invoke(call);
    if (!result.ok) {
        if (args.warn) args.warn("批准 LLM エラー (" + position.stance + "): " + result.error + " — reject デフォルト");
        return reject_default("批准 LLM 障害 (" + result.error + ")");
    }
    const auto obj = extract_json_object(result.text);
    if (!obj) {
        if (args.warn) args.warn("批准 JSON パース失敗 (" + position.stance + ") — reject デフォルト");
        return reject_default("批准応答を解析できず reject (デフォルト)");
    }
    auto own_list = [&](const Json& v) {
        std::vector<std::string> out;
        for (const auto& s : v)
            if (s.is_string()) {
                auto t = str::trim(s.get<std::string>());
                if (is_own(t)) out.push_back(std::move(t));
            }
        return out;
    };
    const Json& p = field(*obj, "preservedIds");
    const std::vector<std::string> preserved = p.is_array() ? own_list(p) : std::vector<std::string>{};
    const Json& m = field(*obj, "missing");
    std::vector<std::string> missing;
    if (m.is_array()) missing = own_list(m);
    else
        for (const auto& id : own_ids)
            if (std::find(preserved.begin(), preserved.end(), id) == preserved.end()) missing.push_back(id);
    const Json& r = field(*obj, "reason");
    const std::string reason = r.is_string() ? r.get<std::string>() : "";

    // 機械検証: 過半数保存が確認できたときのみ accept が有効。
    const bool majority = static_cast<double>(preserved.size()) > static_cast<double>(position.grounds.size()) / 2;
    const Json& v = field(*obj, "verdict");
    const bool said_accept = v.is_string() && v.get<std::string>() == "accept";
    if (said_accept && !majority && args.warn)
        args.warn("批准 (" + position.stance + "): verdict=accept だが保存確認 " + std::to_string(preserved.size()) + "/" +
                  std::to_string(position.grounds.size()) + " は過半数未満 — コードが reject に矯正");
    return {position.id, said_accept && majority ? "accept" : "reject", preserved, missing, reason};
}

SynthesisOutcome run_synthesis_loop(const SynthesisLoopArgs& args) {
    if (!args.genLlm || !args.gateLlm || !args.ratifyLlm) throw std::invalid_argument("synthesis loop needs gen/gate/ratify LLMs");
    auto name_of = [&](const PositionRecord& p) {
        auto it = args.personaNames.find(p.personaId);
        return it == args.personaNames.end() ? p.personaId : it->second;
    };
    std::optional<std::string> record_id;
    std::vector<std::string> missing_feedback;
    SynthesisRecord last;

    for (int revision = 0; revision <= MAX_REVISIONS; ++revision) {
        GenerateSynthesisArgs gen;
        gen.issueTitle = args.issueTitle;
        gen.tension = args.tension;
        gen.positionA = args.positionA;
        gen.positionB = args.positionB;
        if (!missing_feedback.empty()) gen.missingFeedback = missing_feedback;
        gen.paperSystem = args.paperSystem;
        gen.model = args.mainModel;
        gen.warn = args.warn;
        const auto candidate = generate_synthesis(*args.genLlm, gen);
        const std::string kind = elevation_gate(*args.gateLlm, candidate.elevates, args.judgeModel, args.warn);

        std::vector<RatificationEntry> ratifications;
        for (const PositionRecord* position : {&args.positionA, &args.positionB}) {
            RatifyArgs ra;
            ra.position = *position;
            ra.personaName = name_of(*position);
            ra.synthesisText = candidate.text;
            ra.preserves = candidate.preserves;
            ra.elevates = candidate.elevates;
            ra.model = args.judgeModel;
            ra.warn = args.warn;
            ratifications.push_back(ratify_synthesis(*args.ratifyLlm, ra));
        }
        const bool all_accepted =
            std::all_of(ratifications.begin(), ratifications.end(), [](const auto& r) { return r.verdict == "accept"; });
        const std::string status = all_accepted ? "ratified" : (revision < MAX_REVISIONS ? "revising" : "rejected");

        SynthesisRecord snapshot;
        snapshot.tensionId = args.tension.id;
        snapshot.preserves = candidate.preserves;
        snapshot.negates = candidate.negates;
        snapshot.elevates = candidate.elevates;
        snapshot.kind = kind;
        snapshot.text = candidate.text;
        snapshot.ratification = ratifications;
        snapshot.status = status;
        snapshot.revision = revision;
        if (!record_id) {
            last = insert_synthesis(snapshot);
            record_id = last.id;
        } else {
            update_synthesis(*record_id, snapshot);
            snapshot.id = *record_id;
            last = snapshot;
        }
        if (all_accepted) return {last, kind == "aufheben" ? "synthesized" : "compromised"};

        missing_feedback.clear();
        for (const auto& r : ratifications)
            if (r.verdict == "reject") missing_feedback.insert(missing_feedback.end(), r.missing.begin(), r.missing.end());
        if (args.warn)
            args.warn("Synthesis 批准 reject (revision " + std::to_string(revision) + "/" + std::to_string(MAX_REVISIONS) +
                      "): 欠落根拠 [" + str::join(missing_feedback, ", ") + "]" +
                      (revision < MAX_REVISIONS ? " — 修正ループへ" : " — 打ち切り (agreed_disagree)"));
    }
    // MAX_REVISIONS を使い切って批准不成立 → 合意不能として記録 (握り潰さない)。
    return {last, "agreed_disagree"};
}

} // namespace di::flow::dialectic
