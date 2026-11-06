#include "discatier_engine_adapter/voice_search.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "core/noise/exclusions.hpp"
#include "core/vectors/hybrid_rank.hpp"
#include "core/vectors/query_embed_cache.hpp"
#include "core/vectors/vector_math.hpp"
#include "core/vectors/vector_search.hpp"
#include "discatier_engine_adapter/game_aliases.hpp"
#include "discatier_engine_adapter/keyword_terms.hpp"
#include "discatier_engine_adapter/ludus_term_aliases.hpp"
#include "discord_hook/reactions.hpp"

namespace di::discatier_engine_adapter {

namespace {

namespace vec = core::vectors;

/** 候補・games 一覧の行数・バイト数の上限 (DB の値も外部入力)。行数は SQL の LIMIT が先に効く。 */
constexpr sql::QueryLimits kCandidateLimits{static_cast<std::size_t>(KEYWORD_CAP) + 1, 256u << 20};
constexpr sql::QueryLimits kGamesLimits{200'000, 64u << 20};
/** RRF 融合の系別重み (キーワード : ベクトル)。 */
const std::vector<double> RRF_WEIGHTS{1.0, 1.0};

std::mutex g_embedder_mutex;
std::shared_ptr<vec::EmbeddingClient> g_embedder;
std::shared_ptr<vec::EmbeddingClient> g_embedder_override;

vec::EmbeddingConfig embedding_config_of(const config::DiscutereConfig& c) {
    vec::EmbeddingConfig e;
    e.enabled = c.embedding.enabled;
    e.baseUrl = c.embedding.baseUrl;
    e.model = c.embedding.model;
    e.apiKey = c.embedding.apiKey;
    e.timeoutMs = c.embedding.timeoutMs;
    e.batchSize = c.embedding.batchSize;
    return e;
}

/** 設定から作る埋め込みクライアント (プロセスで 1 つ)。作れなければ nullptr (warn してキーワード検索で継続)。 */
std::shared_ptr<vec::EmbeddingClient> get_embedder(const VoiceWarnFn& warn) {
    std::lock_guard lock(g_embedder_mutex);
    if (g_embedder_override) return g_embedder_override;
    if (!g_embedder) {
        try {
            g_embedder = vec::create_open_ai_compat_embedder(embedding_config_of(config::get_config()));
        } catch (const std::exception& e) {
            warn(std::string("埋め込みクライアントを作れない (キーワード検索で継続): ") + e.what());
            return nullptr;
        }
    }
    return g_embedder;
}

/** LIKE のワイルドカード (% _ \) をエスケープして部分一致に倒す。 */
std::string escape_like(std::string_view t) {
    std::string out;
    out.reserve(t.size());
    for (char c : t) {
        if (c == '\\' || c == '%' || c == '_') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

struct VoiceRow {
    std::string id;
    std::optional<std::string> speaker_id;
    std::string raw_content;
};

std::vector<VoiceRow> fetch_candidates(core::Core& core, const std::string& workspace_id, const std::vector<std::string>& key_terms) {
    std::vector<sql::Row> rows;
    if (key_terms.empty()) {
        rows = core.raw().all(
            "SELECT id, speaker_id, raw_content\n"
            "             FROM utterances\n"
            "            WHERE workspace_id = ? AND speaker_id LIKE 'ext:%'\n"
            "            ORDER BY posted_at DESC LIMIT ?",
            {workspace_id, RECENT_CAP}, kCandidateLimits);
    } else {
        std::string like_clauses;
        sql::Params params;
        params.add(workspace_id);
        for (std::size_t i = 0; i < key_terms.size(); ++i) {
            if (i > 0) like_clauses += " OR ";
            like_clauses += "raw_content LIKE ? ESCAPE '\\'";
            params.add("%" + escape_like(key_terms[i]) + "%");
        }
        params.add(KEYWORD_CAP);
        rows = core.raw().all(
            "SELECT id, speaker_id, raw_content\n"
            "           FROM utterances\n"
            "          WHERE workspace_id = ? AND speaker_id LIKE 'ext:%' AND (" +
                like_clauses + ")\n          LIMIT ?",
            params, kCandidateLimits);
    }
    std::vector<VoiceRow> out;
    out.reserve(rows.size());
    for (const auto& r : rows) out.push_back({r.str("id"), r.opt_str("speaker_id"), r.str("raw_content")});
    return out;
}

struct Scored {
    const VoiceRow* r;
    double score;
};

} // namespace

Json to_json(const ScoredVoiceRow& r) {
    Json j = Json::object();
    j["id"] = r.id;
    j["speakerId"] = r.speakerId ? Json(*r.speakerId) : Json(nullptr);
    j["rawContent"] = r.rawContent;
    j["score"] = r.score;
    return j;
}

void set_voice_embedder_for_test(std::shared_ptr<vec::EmbeddingClient> embedder) {
    std::lock_guard lock(g_embedder_mutex);
    g_embedder_override = std::move(embedder);
}

std::vector<std::string> build_search_terms(core::Core& core, const std::string& workspace_id, const std::vector<std::string>& terms) {
    auto key_terms = extract_key_terms(terms);
    if (key_terms.empty()) return key_terms;
    try {
        std::vector<std::string> titles;
        for (const auto& g : core.raw().all("SELECT title FROM games WHERE workspace_id = ?", {workspace_id}, kGamesLimits))
            titles.push_back(g.str("title"));
        key_terms = expand_aliases(key_terms, build_alias_groups(titles, ludus_term_alias_groups()), EXPANDED_TERMS_MAX);
    } catch (const std::exception&) {
        // games 取得に失敗しても分解済みの語で続行 (別名拡張は best-effort)。
    }
    return key_terms;
}

std::vector<ScoredVoiceRow> search_external_voice_rows(core::Core& core, const std::string& workspace_id, const std::vector<std::string>& terms,
                                                       int limit, const VoiceWarnFn& warn_in, const SearchOptions& options) {
    if (limit <= 0) return {};
    const VoiceWarnFn warn = warn_in ? warn_in : VoiceWarnFn([](const std::string& m) { log::warn("[voice-search] " + m); });
    const auto key_terms = build_search_terms(core, workspace_id, terms);

    // opinion_scores はリアクション系テーブル。boot 前/単体呼び出しでも落ちないよう冪等保証。
    discord_hook::ensure_reaction_tables(core.raw());
    const auto excluded = core::noise::list_excluded_ids(core.raw());
    std::vector<VoiceRow> rows;
    for (auto& r : fetch_candidates(core, workspace_id, key_terms))
        if (!excluded.count(r.id)) rows.push_back(std::move(r));
    if (rows.empty()) return {};

    // キーワード合成スコア (従来互換): 一致語数を主、opinion (合意/支持) を従。
    std::vector<Scored> scored;
    for (const auto& r : rows) {
        const std::string lc = str::to_lower(r.raw_content);
        double hits = 0;
        for (const auto& t : key_terms)
            if (str::contains(lc, t)) hits += 1;
        const double opinion = static_cast<double>(discord_hook::get_opinion_score(core.raw(), r.id));
        const double score = hits * 10 + opinion;
        // 関連語ヒットも支持スコアも無い声は議論を薄めるので落とす。
        if (score > 0) scored.push_back({&r, score});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return b.score < a.score; });
    if (scored.empty()) return {};

    const auto& embedding_cfg = config::get_config().embedding;
    std::vector<Scored> ordered = scored;
    std::optional<std::map<std::string, std::vector<double>>> vectors;

    if (options.useEmbedding.value_or(true) && embedding_cfg.enabled && !terms.empty()) {
        const std::string query_text = vec::normalize_query_text(terms);
        const auto query_vec = vec::get_cached_query_vector(core.raw(), embedding_cfg.model, query_text);
        if (query_vec) {
            const std::size_t pool_size = std::min(scored.size(), RERANK_CAP);
            vec::FetchVectorsInput input;
            input.workspaceId = workspace_id;
            input.nodeType = "utterance";
            for (std::size_t i = 0; i < pool_size; ++i) input.nodeIds.push_back(scored[i].r->id);
            auto candidate_vectors = vec::fetch_vectors_by_node_ids(core.raw(), input);
            // 部分索引では「ベクトル有り」自体が加点になり、強いキーワード候補を落とす。
            // rerank 対象が揃うまではキーワード順へ degrade する。
            bool complete = true;
            for (std::size_t i = 0; i < pool_size; ++i)
                if (!candidate_vectors.count(scored[i].r->id)) complete = false;
            if (complete) {
                struct Sim {
                    std::string id;
                    double sim;
                };
                std::vector<Sim> sims;
                for (std::size_t i = 0; i < pool_size; ++i) {
                    const auto& id = scored[i].r->id;
                    sims.push_back({id, vec::cosine_similarity(*query_vec, candidate_vectors.at(id))});
                }
                std::stable_sort(sims.begin(), sims.end(), [](const Sim& a, const Sim& b) { return b.sim < a.sim; });
                std::vector<std::string> vector_rank;
                for (const auto& s : sims) vector_rank.push_back(s.id);
                std::vector<std::string> keyword_rank;
                for (const auto& s : scored) keyword_rank.push_back(s.r->id);
                const auto fused = vec::rrf_fuse({keyword_rank, vector_rank}, RRF_WEIGHTS);
                std::unordered_map<std::string, double> fused_of;
                for (const auto& [id, v] : fused.entries()) fused_of.emplace(id, v);
                auto fused_score = [&](const Scored& s) {
                    const auto it = fused_of.find(s.r->id);
                    return it == fused_of.end() ? 0.0 : it->second;
                };
                std::stable_sort(ordered.begin(), ordered.end(),
                                 [&](const Scored& a, const Scored& b) { return fused_score(b) < fused_score(a); });
                vectors = std::move(candidate_vectors);
            }
        } else {
            // 初回 miss: 裏で温めて次回からハイブリッド化 (今回はキーワード順で返す)。
            // background_executor が満杯なら warm_query_embedding が warn して温めない。
            if (auto embedder = get_embedder(warn))
                vec::warm_query_embedding(core.raw_ptr(), embedder, embedding_cfg.model, query_text, warn);
        }
    }

    // 近似重複を畳んでから MMR で多様化して limit 件に絞る。
    std::vector<std::string_view> contents;
    contents.reserve(ordered.size());
    for (const auto& s : ordered) contents.emplace_back(s.r->raw_content);
    std::vector<const Scored*> deduped;
    for (std::size_t i : vec::dedupe_near_duplicate_indices(contents)) deduped.push_back(&ordered[i]);

    std::vector<const Scored*> picked;
    if (vectors && !vectors->empty()) {
        const double max_fused_rank = static_cast<double>(deduped.size());
        std::vector<vec::MmrCandidate> candidates;
        std::unordered_map<std::string, const Scored*> by_id;
        for (std::size_t i = 0; i < deduped.size(); ++i) {
            vec::MmrCandidate c;
            c.id = deduped[i]->r->id;
            // MMR の relevance は順位ベースで 1..0 に正規化 (スコアのスケール非依存)。
            c.relevance = (max_fused_rank - static_cast<double>(i)) / max_fused_rank;
            const auto it = vectors->find(c.id);
            if (it != vectors->end()) c.vector = it->second;
            by_id.emplace(c.id, deduped[i]);
            candidates.push_back(std::move(c));
        }
        for (const auto& c : vec::mmr_select(candidates, limit, MMR_LAMBDA)) {
            const auto it = by_id.find(c.id);
            if (it != by_id.end()) picked.push_back(it->second);
        }
    } else {
        for (std::size_t i = 0; i < deduped.size() && i < static_cast<std::size_t>(limit); ++i) picked.push_back(deduped[i]);
    }

    std::vector<ScoredVoiceRow> out;
    out.reserve(picked.size());
    for (const auto* s : picked) out.push_back({s->r->id, s->r->speaker_id, s->r->raw_content, s->score});
    return out;
}

} // namespace di::discatier_engine_adapter
