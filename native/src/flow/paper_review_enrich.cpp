#include "flow/paper_review_enrich.hpp"

#include "base/strings.hpp"
#include "flow/paper_review.hpp"

namespace di::flow {

namespace {

void warn_if(const WarnFn& warn, const std::string& msg) {
    if (warn) warn(msg);
}

PaperIntentInstruction build_voices_instruction(const PaperReviewIntent& intent, const PaperIntentDeps& deps) {
    std::string query = str::join(intent.terms, " ");
    if (query.empty()) query = deps.theme;
    int imported = 0;
    if (deps.crawl) {
        try {
            imported = deps.crawl(query);
        } catch (const std::exception& e) {
            warn_if(deps.warn, "外部の声のクロール失敗 (" + query + "): " + e.what());
        }
    }
    if (!deps.listExternalVoices) return {std::nullopt, "外部の声の検索が使えないため、取り込めませんでした。"};
    GatherEvidenceArgs ga;
    ga.topic = query;
    ga.listExternalVoices = deps.listExternalVoices;
    ga.llm = deps.llm;
    ga.warn = deps.warn;
    const auto evidence = gather_evidence(ga);
    const std::string crawled = imported > 0 ? "新たに " + std::to_string(imported) + " 件をクロールして取り込み、" : "";
    if (evidence.voices.empty()) return {std::nullopt, crawled + "「" + query + "」に関する外部の声は見つかりませんでした。"};
    return {intent.text + "\n\n" +
                "次の外部の声 (出所付き) を根拠として、ペーパーの該当箇所 (観点補足 / ゲーム内容 / 論点) に取り込んでください。" +
                "出所は残し、個人名は書かないこと。\n\n" + evidence.suggestion,
            crawled + "「" + query + "」の外部の声 " + std::to_string(evidence.voices.size()) + " 件を根拠として反映します。"};
}

PaperIntentInstruction build_similar_games_instruction(const PaperReviewIntent& intent, const PaperIntentDeps& deps) {
    const InvestigateFn investigate = deps.investigate ? deps.investigate : InvestigateFn(investigate_theme);
    std::vector<std::string> games;
    for (std::size_t i = 0; i < intent.terms.size() && i < kMaxSimilarGames; ++i) games.push_back(intent.terms[i]);
    std::vector<std::string> found;
    std::vector<std::string> missing;
    for (const auto& game : games) {
        try {
            InvestigateArgs args;
            args.theme = game;
            args.tags = deps.tags;
            if (deps.gamesDir) args.gamesDir = *deps.gamesDir;
            args.warn = deps.warn;
            const auto r = investigate(args);
            if (r.mechanics.empty()) {
                missing.push_back(game);
                continue;
            }
            for (std::size_t i = 0; i < r.mechanics.size() && i < kMaxMechanicsPerGame; ++i) {
                const auto& m = r.mechanics[i];
                found.push_back("- [" + game + "] " + m.name + (m.description.empty() ? "" : ": " + m.description));
            }
        } catch (const std::exception& e) {
            warn_if(deps.warn, "類似ゲームのメカニクス取得失敗 (" + game + "): " + e.what());
            missing.push_back(game);
        }
    }

    std::vector<std::string> lines{intent.text, "",
                                   std::string("類似するゲームのメカニクスを、このゲームのメカニクスの代替 (参考) として「メカニクス」に追加してください。") +
                                       "追加する項目の説明には「(参考: ゲーム名)」を付け、このゲーム本来のメカニクスと区別すること。"};
    if (!found.empty()) {
        lines.push_back("");
        lines.push_back("既存データにある類似ゲームのメカニクス:");
        lines.insert(lines.end(), found.begin(), found.end());
    }
    if (!missing.empty()) {
        lines.push_back("");
        lines.push_back("既存データに無いゲーム (" + str::join(missing, "、") + ") は、代表的なメカニクスを一般知識から補ってください。");
    }
    if (games.empty()) {
        lines.push_back("");
        lines.push_back("ゲーム名の指定が無いため、議題に近い類似ゲームを 1〜3 本選び、その代表的なメカニクスを補ってください。");
    }
    const std::string notice =
        games.empty() ? "類似ゲームの指定が無いため、議題に近いゲームを選んでメカニクスの代替を補います。"
                      : "類似ゲーム (" + str::join(games, "、") + ") のメカニクスを代替として反映します" +
                            (found.empty() ? std::string() : " (既存データ " + std::to_string(found.size()) + " 件)") + "。";
    return {str::join(lines, "\n"), notice};
}

} // namespace

Json to_json(const PaperIntentInstruction& r) {
    return Json{{"instruction", r.instruction ? Json(*r.instruction) : Json(nullptr)}, {"notice", r.notice}};
}

PaperIntentInstruction build_intent_instruction(const PaperReviewIntent& intent, const PaperIntentDeps& deps) {
    if (intent.kind == "voices") return build_voices_instruction(intent, deps);
    if (intent.kind == "similar_games") return build_similar_games_instruction(intent, deps);
    return {intent.text, ""};
}

} // namespace di::flow
