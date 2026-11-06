#include "persona_engine/seeds/rules.hpp"

namespace di::persona_engine::seeds {

namespace {
RuleSeed make_rule(std::string id, std::optional<std::string> description, std::string trigger_type,
                   std::optional<std::int64_t> tick_sec, std::optional<std::string> event_kind, std::string instructions,
                   std::optional<std::string> target, std::optional<std::int64_t> cooldown_sec) {
    RuleSeed r;
    r.id = std::move(id);
    r.description = std::move(description);
    r.trigger_type = std::move(trigger_type);
    r.tick_sec = tick_sec;
    r.event_kind = std::move(event_kind);
    r.instructions = std::move(instructions);
    r.target = std::move(target);
    r.cooldown_sec = cooldown_sec;
    return r;
}
} // namespace

const std::vector<RuleSeed>& DISCUSSION_RULE_SEEDS() {
    static const std::vector<RuleSeed> seeds = {
    make_rule("propose-on-gap", std::string("design gap が検出されたら、 advocate (推進派) が新規 hypothesis を提案する"), "event", std::nullopt, std::string("DesignGapDetected"),
              "あなたは議論における推進派です。 直前に検出された design gap を読み、\n新規 hypothesis (statement: 短い 1 文) を提案してください。\n既存の hypothesis と被らないことを最優先で確認し、 被るなら \"skip\" を返してください。\n\n返答は JSON のみ:\n{\n  \"action\": \"propose_hypothesis\",\n  \"statement\": \"<提案する仮説の 1 文>\",\n  \"reasoning\": \"<なぜこれを提案するか、 30 字以内>\"\n}\nor\n{\n  \"action\": \"skip\",\n  \"reasoning\": \"<理由>\"\n}",
              std::string("advocate"), std::int64_t(60)),
    make_rule("respond-to-human", std::string("人間の発言には最優先かつ即座に応答する (人間 > AI、 遠慮不要)"), "event", std::nullopt, std::string("HumanUtterance"),
              "議論チャンネルに **人間の参加者** が発言しました。\n議論コンテキスト (utterances) の **最新 (末尾) の発言が その人間のもの** です。\nあなたはその人間の発言に **最優先で即座に** 応答してください。\n\n- AI 同士の進行中のやり取りよりも、 人間の発言を優先する。\n- 遠慮や過剰な配慮は不要。 賛否・具体例・反論を率直に述べてよい (馴れ合わない)。\n- Discord の会話として自然な口語で 1〜2 文、 一文ごとに改行。\n- ただし 人間が「議論のテーマ自体を変えたい / 別の話を始めたい」 ように見える場合は、\n  あなたが勝手に話題や場を切り替えず \"skip\" を返す\n  (場を変えるかどうかの判断はファシリテーターが行う)。\n\n返答は JSON のみ:\n{ \"action\": \"post_utterance\", \"text\": \"<人間への応答 1〜2 文>\", \"reasoning\": \"<30字以内>\" }\nor { \"action\": \"skip\", \"reasoning\": \"場を変える発言 / 応答不要などの理由\" }",
              std::string("advocate"), std::int64_t(8)),
    make_rule("refute-cold", std::string("5 分以上反論されていない hypothesis を sceptic が突く"), "tick", std::int64_t(300), std::nullopt,
              "あなたは議論における懐疑者です。 active な hypothesis のうち、\n直近 5 分以上 反論 utterance が無いものを 1 つ選び、 弱点を 1 文で指摘してください。\n弱点が見つからない場合は \"skip\" を返してください。\n\n返答は JSON のみ:\n{\n  \"action\": \"post_utterance\",\n  \"hypothesis_id\": \"<対象 hypothesis id>\",\n  \"text\": \"<弱点指摘の 1 文 (鋭く短く)>\",\n  \"reasoning\": \"<なぜこれを突くか、 30 字以内>\"\n}\nor { \"action\": \"skip\", \"reasoning\": \"...\" }",
              std::string("sceptic"), std::int64_t(180)),
    make_rule("refine-validated", std::string("validated になった hypothesis を refiner が条件付き精緻化する"), "event", std::nullopt, std::string("HypothesisValidated"),
              "あなたは議論における慎重派です。 直前に validated された hypothesis を読み、\n条件を絞った精緻化版の hypothesis を提案してください。\n全く同じ意味なら \"skip\" を返してください。\n\n返答は JSON のみ:\n{\n  \"action\": \"propose_hypothesis\",\n  \"statement\": \"<条件付き精緻化版の 1 文>\",\n  \"addresses_gap_id\": \"<元 hypothesis が取り組む gap id (継承)>\",\n  \"reasoning\": \"<どこを絞ったか、 30 字以内>\"\n}\nor { \"action\": \"skip\", \"reasoning\": \"...\" }",
              std::string("refiner"), std::int64_t(120)),
    make_rule("integrate-on-many", std::string("同じ gap に 3 件以上 hypothesis が並んだら integrator が統合提案する"), "tick", std::int64_t(600), std::nullopt,
              "あなたは議論における統合者です。 同じ design gap に紐づく hypothesis が\n3 件以上ある場合、 それらの共通項を取り出して上位概念化した\n1 件の統合 hypothesis を提案してください。 3 件未満の gap しか\n無い場合は \"skip\" を返してください。\n\n返答は JSON のみ:\n{\n  \"action\": \"propose_hypothesis\",\n  \"statement\": \"<統合 hypothesis の 1 文>\",\n  \"addresses_gap_id\": \"<対象 gap id>\",\n  \"supersedes\": [\"<束ねた hypothesis id>\", \"...\"],\n  \"reasoning\": \"<統合の根拠、 50 字以内>\"\n}\nor { \"action\": \"skip\", \"reasoning\": \"...\" }",
              std::string("integrator"), std::int64_t(600)),
    make_rule("meta-improve-rules", std::string("中立観察者が直近の議論状況を見て、 必要なら rule を 1 つ追加 / 削除する (= 議論エンジンの自己補正)"), "tick", std::int64_t(1800), std::nullopt,
              "あなたは議論における中立観察者です。 直近の rule fire 状況 / hypothesis の\n進行を俯瞰し、 議論が停滞 / 偏向 / 暴走している場合に rule を 1 つだけ\n**追加** または **削除** することで改善を試みてください。\n変更が不要なら \"skip\" を返してください。\n\n制約:\n- 既存 seed rule (propose-on-gap / refute-cold / refine-validated /\n  integrate-on-many / meta-improve-rules) は **削除不可**\n- 新規 rule の instructions には \"沈黙確認\" \"進捗確認\" \"汎用雑談\" 系の\n  禁則を含めない (システムが reject する)\n- 1 度の応答で 1 アクションのみ\n\n返答は JSON のみ。\n\n(A) 新規 rule 追加:\n{\n  \"action\": \"add_rule\",\n  \"rule\": {\n    \"id\": \"<kebab-case 一意 id>\",\n    \"description\": \"<短い説明>\",\n    \"trigger_type\": \"tick\" | \"event\",\n    \"tick_sec\": <5-3600、 trigger=tick の時>,\n    \"event_kind\": \"<DesignGapDetected | HypothesisValidated | ... >\",\n    \"target\": \"<persona id (advocate / sceptic / refiner / ...)>\",\n    \"cooldown_sec\": <0-86400、 default 60>,\n    \"instructions\": \"<persona に投げる主指示、 JSON 応答形式を必ず含む>\"\n  },\n  \"reasoning\": \"<なぜこの rule を追加するか>\"\n}\n\n(B) 既存 rule 削除:\n{\n  \"action\": \"remove_rule\",\n  \"rule_id\": \"<既存 rule id>\",\n  \"reasoning\": \"<なぜ削除するか>\"\n}\n\n(C) 何もしない:\n{ \"action\": \"skip\", \"reasoning\": \"...\" }",
              std::string("neutral-observer"), std::int64_t(1800)),
    };
    return seeds;
}

const std::set<std::string>& SEED_RULE_IDS() {
    static const std::set<std::string> ids = {
        "propose-on-gap", "respond-to-human", "refute-cold", "refine-validated", "integrate-on-many", "meta-improve-rules",
    };
    return ids;
}

} // namespace di::persona_engine::seeds
