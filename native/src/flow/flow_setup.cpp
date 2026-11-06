#include "flow/flow_setup.hpp"

#include <stdexcept>

#include "base/strings.hpp"
#include "config.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/project_premise.hpp"
#include "flow/tags.hpp"
#include "flow/user_voices/reference_policy.hpp"

namespace di::flow {

FlowSetupResult setup_flow_paper(persona_engine::LLMClient& llm, const FlowSetupArgs& args) {
    if (!args.voiceCache) throw std::invalid_argument("setup_flow_paper: voiceCache is required");
    const auto& cfg = config::get_config();
    auto log = [&](const std::string& m) {
        if (args.log) args.log(m);
    };
    FlowSetupResult out;
    std::vector<MechanicSummary> mechanics;
    std::string supplement;
    std::optional<std::string> body_md;

    // ── [1] 調査 ── 確定ペーパー (人間レビュー済) があれば investigate を省略し、その内容で議論する。
    if (args.paperOverride) {
        mechanics = args.paperOverride->mechanics;
        supplement = args.paperOverride->supplement;
        body_md = args.paperOverride->bodyMd;
        log("確定ペーパー使用: investigate スキップ (メカニクス " + std::to_string(mechanics.size()) + " 件)");
    } else if (args.flow == "discussion") {
        // 企画のテーマを既存作品名とみなして仕様を取り込まない。
        supplement = paper_supplement(args.tags);
    } else {
        log("調査開始: \"" + args.theme + "\" (タグ: [" + str::join(args.tags, ", ") + "])");
        InvestigateArgs inv;
        inv.theme = args.theme;
        inv.tags = args.tags;
        if (args.gamesDir) inv.gamesDir = *args.gamesDir;
        inv.youtubeSearch = args.youtubeSearch;
        inv.youtubeMaxComments = static_cast<int>(cfg.flow.youtubeMaxComments);
        inv.warn = args.warn;
        out.investigation = investigate_theme(inv);
        mechanics = out.investigation->mechanics;
        supplement = paper_supplement(args.tags);
        // メカニクスを LLM で目標件数まで増補 (材料 = 感想が無ければ増補しない)。
        const auto enrich_voices = args.voiceCache->lookup({args.theme}, static_cast<int>(cfg.flow.paperRichness.voices));
        if (cfg.flow.paperRichness.enrichMechanics && !enrich_voices.empty()) {
            EnrichMechanicsArgs e;
            e.theme = args.theme;
            e.existing = mechanics;
            for (const auto& v : enrich_voices) e.voices.push_back({v.content, v.source});
            e.target = static_cast<int>(cfg.flow.paperRichness.mechanicsTarget);
            if (!cfg.flow.paperRichness.enrichModel.empty()) e.model = cfg.flow.paperRichness.enrichModel;
            e.warn = args.warn;
            mechanics = enrich_mechanics(llm, e);
            log("メカニクス増補後: " + std::to_string(mechanics.size()) + " 件");
        }
    }

    // ── [2] ディスカッションペーパー初期化 ── bodyMd が未指定なら構造化フィールドから生成する。
    if (!body_md || str::trim(*body_md).empty()) {
        PaperContent content;
        content.theme = args.theme;
        content.tags = args.tags;
        content.supplement = supplement;
        content.mechanics = mechanics;
        body_md = paper_draft_to_markdown(content);
    }
    out.paperId = persist_paper({args.sessionId, args.theme, args.tags, mechanics, supplement, body_md}, args.flow);
    out.paper.paperId = out.paperId;
    out.paper.sessionId = args.sessionId;
    out.paper.theme = args.theme;
    out.paper.tags = args.tags;
    out.paper.mechanics = mechanics;
    out.paper.supplement = supplement;
    // 確定本文の保存値は維持し、両議論エンジンへ現在のモード前提を渡す。
    std::vector<std::string> parts;
    for (std::string p : {project_premise(args.flow), std::string(user_voices::VOICE_REFERENCE_POLICY), *body_md})
        if (!p.empty()) parts.push_back(std::move(p));
    out.paper.bodyMd = str::join(parts, "\n\n");
    return out;
}

} // namespace di::flow
