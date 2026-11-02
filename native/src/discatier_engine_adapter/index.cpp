#include "discatier_engine_adapter/index.hpp"

#include <stdexcept>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/persona.hpp"
#include "discatier_engine_adapter/voice_search.hpp"
#include "discord_hook/facilitator_directives.hpp"
#include "visualize/dump.hpp"

namespace di::discatier_engine_adapter {

namespace {

namespace pe = persona_engine;

/** 一覧の行数・バイト数の上限 (DB の値も外部入力。行数は SQL の LIMIT が先に効く)。 */
constexpr sql::QueryLimits kListLimits{200'000, 128u << 20};

class DiscatierContextProvider final : public pe::DiscussionContextProvider {
public:
    DiscatierContextProvider(core::Core& core, DiscatierAdapterOptions options) : core_(core), options_(std::move(options)) {
        if (!options_.onAutoExportError) {
            options_.onAutoExportError = [](const std::string& err, const std::string& type, const std::string& id) {
                log::warn("[adapter:auto-export] {type:" + type + ", id:" + id + "} " + err);
            };
        }
    }

    std::vector<pe::ContextHypothesis> list_active_hypotheses(const std::string& workspace_id, int limit) override {
        // gap が closed/converged になった議論の hypothesis は context から外す。
        // これを残すと facilitator が収束させても refute-cold / integrate-on-many
        // (tick rule) がそれらを拾い続け、議論が止まらない (= 収束無効化)。
        const auto rows = core_.raw().all(
            "SELECT h.id, h.statement, h.status, h.design_gap_id, h.updated_at\n"
            "             FROM hypotheses h\n"
            "             LEFT JOIN design_gaps g ON g.id = h.design_gap_id\n"
            "             WHERE h.workspace_id = ?\n"
            "               AND (h.status IS NULL OR h.status NOT IN ('rejected'))\n"
            "               AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged'))\n"
            "             ORDER BY h.updated_at DESC LIMIT ?",
            {workspace_id, limit}, kListLimits);
        std::vector<pe::ContextHypothesis> out;
        for (const auto& r : rows) {
            pe::ContextHypothesis h;
            h.id = r.str("id");
            h.statement = r.str("statement");
            h.status = r.is_null("status") ? std::string("proposed") : r.str("status");
            h.designGapId = r.opt_str("design_gap_id");
            h.updatedAt = r.i64("updated_at");
            out.push_back(std::move(h));
        }
        return out;
    }

    std::vector<pe::ContextGap> list_recent_gaps(const std::string& workspace_id, int limit) override {
        const auto rows = core_.raw().all(
            "SELECT id, title, description, status, gap_in, expected_affect, observed_affect, updated_at\n"
            "             FROM design_gaps\n"
            "             WHERE workspace_id = ?\n"
            "             ORDER BY updated_at DESC LIMIT ?",
            {workspace_id, limit}, kListLimits);
        std::vector<pe::ContextGap> out;
        for (const auto& r : rows) {
            pe::ContextGap g;
            g.id = r.str("id");
            g.title = r.str("title");
            g.description = r.opt_str("description");
            g.status = r.is_null("status") ? std::string("open") : r.str("status");
            g.gapIn = r.opt_str("gap_in");
            g.expectedAffect = r.opt_str("expected_affect");
            g.observedAffect = r.opt_str("observed_affect");
            for (const auto& h : core_.raw().all("SELECT id FROM hypotheses WHERE workspace_id = ? AND design_gap_id = ?", {workspace_id, g.id},
                                                 kListLimits))
                g.hypothesisIds.push_back(h.str("id"));
            g.updatedAt = r.i64("updated_at");
            out.push_back(std::move(g));
        }
        return out;
    }

    std::vector<pe::ContextUtterance> list_recent_utterances(const std::string& workspace_id, const std::string& session_id, int limit) override {
        const auto rows = core_.raw().all(
            "SELECT id, session_id, raw_content, posted_at\n"
            "             FROM utterances\n"
            "             WHERE workspace_id = ? AND session_id = ?\n"
            "             ORDER BY posted_at DESC LIMIT ?",
            {workspace_id, session_id, limit}, kListLimits);
        std::vector<pe::ContextUtterance> out;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) out.push_back({it->str("id"), it->str("session_id"), it->str("raw_content"), it->i64("posted_at")});
        return out;
    }

    std::vector<pe::ContextExternalVoice> list_relevant_external_voices(const std::string& workspace_id, const std::vector<std::string>& terms,
                                                                        int limit) override {
        if (limit <= 0) return {};
        // 検索本体 (キーワード recall + ベクトル rerank + 多様化) は voice_search。
        // ここは attribution (出所透過) と個人マスク (§6) だけを担う。
        const auto rows = search_external_voice_rows(core_, workspace_id, terms, limit);
        if (rows.empty()) return {};

        auto attribution = crawler::sources::open_attribution_store();
        std::vector<pe::ContextExternalVoice> out;
        try {
            for (const auto& r : rows) {
                const auto a = attribution->get(r.id);
                pe::ContextExternalVoice v;
                v.id = r.id;
                v.content = str::js_length(r.rawContent) > EXTERNAL_VOICE_CONTENT_CAP
                                ? str::js_slice(r.rawContent, 0, static_cast<std::int64_t>(EXTERNAL_VOICE_CONTENT_CAP)) + "…"
                                : r.rawContent;
                v.source = a ? a->source : std::string("external");
                v.sourceUrl = a ? std::optional<std::string>(a->sourceUrl) : std::nullopt;
                // §6: 個人 (speaker_id = 公開 ID) はペルソナ名へマスク。公開 ID は出さない。
                v.speakerLabel = (r.speakerId && !r.speakerId->empty()) ? crawler::sources::masked_persona_label(*r.speakerId) : std::string("外部の声");
                v.score = r.score;
                out.push_back(std::move(v));
            }
        } catch (...) {
            attribution->close();
            throw;
        }
        attribution->close();
        return out;
    }

    std::vector<std::string> list_facilitator_directives(const std::string& workspace_id, const std::string& gap_id, int limit) override {
        // サイドカー表は list_facilitator_directives 内で冪等確保される (boot 前でも落ちない)。
        return discord_hook::list_facilitator_directives(core_.raw(), workspace_id, gap_id, limit);
    }

    std::string propose_hypothesis(const pe::ProposeHypothesisInput& input) override {
        core::repositories::HypothesisInput h;
        h.workspaceId = input.workspaceId;
        h.designGapId = input.designGapId;
        h.statement = input.statement;
        h.status = "proposed";
        const std::string id = core_.repos.hypothesis.create(h);
        // ひとまず raw insert で hypothesis row を即時参照可能にする。
        const auto now = time::now_ms();
        core_.raw().run(
            "INSERT OR IGNORE INTO hypotheses\n"
            "            (id, workspace_id, design_gap_id, statement, status, integrated, created_at, updated_at)\n"
            "           VALUES (?, ?, ?, ?, 'proposed', 0, ?, ?)",
            {id, input.workspaceId, input.designGapId, input.statement, now, now});
        auto_export_if_enabled("hyp", id, input.workspaceId);
        if (options_.onPostedHypothesis) {
            try {
                options_.onPostedHypothesis({input.workspaceId, input.statement, input.byPersonaId, input.designGapId, id});
            } catch (const std::exception& e) {
                options_.onAutoExportError(e.what(), "hyp", id);
            } catch (...) {
                options_.onAutoExportError("unknown error", "hyp", id);
            }
        }
        return id;
    }

    std::optional<std::string> find_discussion_session(const pe::FindDiscussionSessionInput& input) override {
        std::optional<std::string> gap_id = input.gapId && !input.gapId->empty() ? input.gapId : std::nullopt;
        if (!gap_id && input.hypothesisId && !input.hypothesisId->empty()) {
            const auto row = core_.raw().get("SELECT design_gap_id FROM hypotheses WHERE id = ?", {*input.hypothesisId});
            if (row) gap_id = row->opt_str("design_gap_id");
        }
        if (!gap_id || gap_id->empty()) return std::nullopt;
        // closed/converged gap の議論には tick rule を着地させない (= 収束後の沈黙)。
        const auto gap = core_.raw().get("SELECT status FROM design_gaps WHERE id = ?", {*gap_id});
        if (gap) {
            const std::string status = gap->str("status");
            if (status == "closed" || status == "converged" || status == "dismissed") return std::nullopt;
        }
        // ended_at が入った session (= 管理画面から「閉じた」議論) には着地させない。
        const auto session = core_.raw().get(
            "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? AND ended_at IS NULL ORDER BY started_at DESC LIMIT 1",
            {input.workspaceId, "discussion-of-gap:" + *gap_id});
        if (!session) return std::nullopt;
        return session->opt_str("id");
    }

    std::optional<std::string> find_active_discussion_session(const std::string& workspace_id) override {
        // hypothesis も sessionId も無い純 debate 発話の着地先。進行中 (未終了・未収束)
        // の discord-bound 議論 session のうち最新のものを 1 件返す。
        const auto row = core_.raw().get(
            "SELECT s.id AS id\n"
            "             FROM sessions s\n"
            "             JOIN design_gaps g\n"
            "               ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)\n"
            "            WHERE s.workspace_id = ?\n"
            "              AND s.title LIKE 'discussion-of-gap:%'\n"
            "              AND s.scene LIKE 'discord:%'\n"
            "              AND s.ended_at IS NULL\n"
            "              AND (g.status IS NULL OR g.status NOT IN ('closed','converged','dismissed','resolved','rejected'))\n"
            "            ORDER BY s.started_at DESC LIMIT 1",
            {workspace_id});
        if (!row) return std::nullopt;
        return row->opt_str("id");
    }

    std::string post_utterance(const pe::PostUtteranceInput& input) override {
        std::string session_id = input.sessionId;
        if (session_id.empty() && options_.defaultSessionId) session_id = *options_.defaultSessionId;
        if (session_id.empty()) throw std::runtime_error("postUtterance requires sessionId or adapter.defaultSessionId");
        core::repositories::UtteranceInput u;
        u.workspaceId = input.workspaceId;
        u.sessionId = session_id;
        u.speakerId = "persona:" + input.byPersonaId;
        u.rawContent = input.text;
        u.postedAt = static_cast<double>(time::now_ms());
        u.respondsTo = input.respondsTo;
        const std::string id = core_.repos.utterance.create(u);
        auto_export_if_enabled("utt", id, input.workspaceId);
        if (options_.onPostedUtterance) {
            try {
                options_.onPostedUtterance({input.workspaceId, session_id, input.text, input.byPersonaId, id});
            } catch (const std::exception& e) {
                options_.onAutoExportError(e.what(), "utt", id);
            } catch (...) {
                options_.onAutoExportError("unknown error", "utt", id);
            }
        }
        return id;
    }

private:
    void auto_export_if_enabled(const std::string& type, const std::string& id, const std::string& workspace_id) {
        if (!options_.autoExportRootDir) return;
        try {
            visualize::dump_node(core_, type, id, {options_.autoExportRootDir, workspace_id});
        } catch (const std::exception& e) {
            options_.onAutoExportError(e.what(), type, id);
        }
    }

    core::Core& core_;
    DiscatierAdapterOptions options_;
};

} // namespace

std::shared_ptr<pe::DiscussionContextProvider> create_discatier_context_provider(core::Core& core, DiscatierAdapterOptions options) {
    return std::make_shared<DiscatierContextProvider>(core, std::move(options));
}

} // namespace di::discatier_engine_adapter
