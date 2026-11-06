#include "persona_engine/scoring/consensus_scorer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "core/noise/exclusions.hpp"
#include "discord_hook/reactions.hpp"
#include "persona_engine/facilitator/facilitator.hpp"
#include "persona_engine/facilitator/js_value.hpp"
#include "persona_engine/index.hpp"

namespace di::persona_engine::scoring {

namespace {

constexpr sql::QueryLimits kRowLimits{200'000, 256u << 20};

bool is_human_speaker(const std::string& speaker_id) {
    return !speaker_id.empty() && !str::starts_with(speaker_id, "persona:") && !str::starts_with(speaker_id, "ext:");
}

/** JS の Math.round (0.5 は +∞ 側)。 */
double js_round(double v) { return std::floor(v + 0.5); }

} // namespace

JsonLimits verdict_json_limits() {
    JsonLimits l;
    l.max_bytes = 1u << 20;
    l.max_elements = 50'000;
    l.max_depth = 32;
    return l;
}

std::optional<std::vector<Verdict>> parse_verdicts(const std::string& text) {
    const auto start = text.find('[');
    if (start == std::string::npos) return std::nullopt;
    const auto end = text.rfind(']');
    if (end == std::string::npos || end < start) return std::nullopt;
    const auto parsed = parse_json(std::string_view(text).substr(start, end - start + 1), verdict_json_limits());
    if (!parsed || !parsed->is_array()) return std::nullopt;
    std::vector<Verdict> out;
    for (const auto& item : *parsed) {
        // JS の `!item || typeof item !== "object"` を通るのは object と配列。配列は .i が無いので下で落ちる。
        if (!item.is_object()) continue;
        const auto i_it = item.find("i");
        const double i = facilitator::js_number_of(i_it == item.end() ? nullptr : &*i_it);
        if (!std::isfinite(i) || std::trunc(i) != i) continue;
        Verdict v;
        v.i = i;
        const auto a = item.find("agree");
        v.agree = a != item.end() && a->is_boolean() && a->get<bool>();
        const auto s_it = item.find("score");
        const double s = facilitator::js_number_of(s_it == item.end() ? nullptr : &*s_it);
        v.score = std::isnan(s) || s == 0 ? 0 : s;  // Number(x) || 0
        if (const auto r = item.find("reasoning"); r != item.end() && r->is_string()) v.reasoning = r->get<std::string>();
        out.push_back(std::move(v));
    }
    return out;
}

ConsensusScorer::ConsensusScorer(ConsensusScorerDeps deps) : deps_(std::move(deps)) {
    if (!deps_.core) throw std::invalid_argument("consensus scorer: core required");
    if (!deps_.llm) throw std::invalid_argument("consensus scorer: llm required");
    if (!deps_.logger) deps_.logger = console_logger();
    raw().exec(R"(CREATE TABLE IF NOT EXISTS consensus_scores (
       utterance_id TEXT PRIMARY KEY,
       agree INTEGER NOT NULL,
       score INTEGER NOT NULL,
       reasoning TEXT,
       created_at INTEGER NOT NULL
     ))");
    // TS は facilitator / reactions 側で確保済みの表を前提にしていた。単独で動かしても落ちないよう冪等に確保する。
    core::noise::ensure_exclusion_table(raw());
    discord_hook::ensure_reaction_tables(raw());
}

ConsensusScorer::~ConsensusScorer() { timer_.stop(); }

/** open な議論 (facilitator と同じ条件) を列挙。 */
std::vector<ConsensusScorer::Discussion> ConsensusScorer::list_active_discussions() {
    const auto rows = raw().all(R"(SELECT s.id AS sessionId, s.title AS title, s.scene AS scene
           FROM sessions s
          WHERE s.title LIKE 'discussion-of-gap:%'
            AND (s.scene LIKE 'discord:%' OR s.scene LIKE 'gap:%'))",
                                {}, kRowLimits);
    std::vector<Discussion> out;
    for (const auto& r : rows) {
        const std::string gap_id = str::js_slice(r.str("title"), static_cast<std::int64_t>(str::js_length("discussion-of-gap:")));
        const auto gap = raw().get("SELECT status FROM design_gaps WHERE id = ?", {gap_id});
        if (!gap) continue;
        const auto st = gap->opt_str("status");
        if (st && (*st == "closed" || *st == "converged" || *st == "dismissed")) continue;
        out.push_back({r.str("sessionId"), gap_id});
    }
    return out;
}

/** 未採点の意見発話 (persona / 人間。ext: と facilitator は除外) を最大 8 件。 */
std::vector<ConsensusScorer::Candidate> ConsensusScorer::candidates_for(const std::string& session_id) {
    const auto rows = raw().all(R"(SELECT u.id AS id, u.speaker_id AS speakerId, u.raw_content AS content
           FROM utterances u
          WHERE u.session_id = ?
            AND u.speaker_id IS NOT NULL
            AND u.speaker_id NOT LIKE 'ext:%'
            AND u.speaker_id <> ?
            AND u.id NOT IN (SELECT utterance_id FROM consensus_scores)
            AND u.id NOT IN (SELECT utterance_id FROM utterance_exclusions)
          ORDER BY u.posted_at ASC
          LIMIT ?)",
                                {session_id, std::string("persona:") + facilitator::FACILITATOR_PERSONA_ID, MAX_CANDIDATES},
                                kRowLimits);
    std::vector<Candidate> out;
    for (const auto& r : rows) {
        const auto content = r.str("content");
        // 【収束】まとめ等のシステム発話は意見ではないので弾く。
        if (str::starts_with(content, "【収束】")) continue;
        const auto sp = r.str("speakerId");
        out.push_back({r.str("id"), sp, content, is_human_speaker(sp)});
    }
    return out;
}

std::string ConsensusScorer::gap_topic(const std::string& gap_id) {
    const auto g = raw().get("SELECT title, description FROM design_gaps WHERE id = ?", {gap_id});
    return g ? str::trim(g->str("title") + "\n" + g->str("description")) : std::string("(不明な議題)");
}

void ConsensusScorer::record_score(const Candidate& c, bool agree, std::int64_t score, const std::string& reasoning) {
    raw().run(R"(INSERT OR REPLACE INTO consensus_scores (utterance_id, agree, score, reasoning, created_at)
         VALUES (?, ?, ?, ?, ?))",
              {c.utteranceId, agree ? 1 : 0, score, reasoning, time::now_ms()});
    // 同意された意見は既存 opinion_scores にも加点 (facilitator の topOpinions が拾う)。
    if (agree && score > 0) {
        raw().run(R"(INSERT INTO opinion_scores (target_id, target_kind, score, updated_at)
             VALUES (?, 'utterance', ?, ?)
           ON CONFLICT(target_id) DO UPDATE SET score = score + excluded.score, updated_at = excluded.updated_at)",
                  {c.utteranceId, score, time::now_ms()});
    }
}

void ConsensusScorer::score_discussion(const Discussion& d) {
    const auto candidates = candidates_for(d.sessionId);
    if (candidates.empty()) return;

    std::vector<std::string> numbered;
    for (std::size_t i = 0; i < candidates.size(); ++i)
        numbered.push_back(std::to_string(i) + ": " + str::js_slice(str::collapse_whitespace(candidates[i].content), 0, 300));
    LLMInvokeArgs args;
    args.model = deps_.model;
    args.maxTokens = 700;
    args.system =
        std::string("あなたは議論の合意形成を評価する中立の審査者です。 各意見について、 この議論の総意 (議題の趣旨と他の意見の流れ) が ") +
        "その意見に同意するかを判定し、 0〜5 の同意スコアを付けます。 発言者が人間か AI かは一切区別しません。 JSON 配列のみを返してください。";
    args.prompt = str::join(
        {
            "議題: " + gap_topic(d.gapId),
            "",
            "意見一覧 (番号付き):",
            str::join(numbered, "\n"),
            "",
            R"(各意見について {"i": 番号, "agree": true/false, "score": 0〜5} を返す。)",
            "agree=議論の総意がその意見に賛同するか。 score=同意の強さ (0=反対/無関係, 5=強い合意)。",
            R"(JSON 配列のみ: [{"i":0,"agree":true,"score":4}, ...])",
        },
        "\n");
    const auto res = deps_.llm->invoke(args);
    if (!res.ok) {
        deps_.logger->warn(Json{{"gap_id", d.gapId}, {"err", res.error}}, "consensus scorer llm failed");
        return;
    }
    const auto verdicts = parse_verdicts(res.text);
    if (!verdicts) {
        deps_.logger->warn(Json{{"gap_id", d.gapId}}, "consensus scorer parse failed");
        return;
    }
    for (const auto& v : *verdicts) {
        if (v.i < 0 || v.i >= static_cast<double>(candidates.size())) continue;
        const auto& c = candidates[static_cast<std::size_t>(v.i)];
        const auto score = static_cast<std::int64_t>(std::max(0.0, std::min(5.0, js_round(v.score))));
        record_score(c, v.agree, score, v.reasoning.value_or(""));
        // 同意なら discord に 👍 (mapping + react がある時のみ)。
        if (v.agree && score > 0 && deps_.react) {
            const auto row = raw().get(
                "SELECT message_id, channel_id FROM discord_message_map WHERE target_id = ? AND target_kind = 'utterance' ORDER BY "
                "created_at DESC LIMIT 1",
                {c.utteranceId});
            if (!row || row->str("message_id").empty() || row->str("channel_id").empty()) continue;
            try {
                deps_.react({c.utteranceId, row->str("channel_id"), row->str("message_id"), CONSENSUS_EMOJI});
            } catch (const std::exception& err) {
                deps_.logger->warn(Json{{"utterance_id", c.utteranceId}, {"err", err.what()}}, "consensus scorer react failed");
            }
        }
    }
    deps_.logger->info(Json{{"gap_id", d.gapId}, {"scored", candidates.size()}}, "consensus scorer scored discussion");
}

void ConsensusScorer::score_once() {
    for (const auto& d : list_active_discussions()) {
        try {
            score_discussion(d);
        } catch (const std::exception& err) {
            deps_.logger->warn(Json{{"gap_id", d.gapId}, {"err", err.what()}}, "consensus scorer discussion failed");
        }
    }
}

void ConsensusScorer::start() {
    timer_.start(
        std::chrono::milliseconds(static_cast<long long>(deps_.intervalMs)),
        [this] {
            try {
                score_once();
            } catch (const std::exception& err) {
                deps_.logger->warn(Json{{"err", err.what()}}, "consensus scorer tick failed");
            }
        },
        [this] { deps_.logger->warn(Json::object(), "consensus scorer tick skipped (executor queue full)"); });
}

void ConsensusScorer::stop() { timer_.stop(); }

std::shared_ptr<ConsensusScorer> create_consensus_scorer(ConsensusScorerDeps deps) {
    return std::make_shared<ConsensusScorer>(std::move(deps));
}

} // namespace di::persona_engine::scoring
