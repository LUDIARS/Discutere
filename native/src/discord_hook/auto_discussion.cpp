#include "discord_hook/auto_discussion.hpp"

#include <array>
#include <stdexcept>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/kg_registry.hpp"

namespace di::discord_hook {

namespace {

namespace pe = persona_engine;

/** gap 一覧の行数・バイト数の上限 (DB の値も外部入力)。 */
constexpr sql::QueryLimits kGapRowLimits{500'000, 512u << 20};

/** /[?？]|とは|なに|何|なぜ|どう|必要|要素|改善|課題|問題|面白|ゲーム|プレイ|ヴァンサバ|vampire/ (大文字小文字を区別する)。 */
constexpr std::array<std::string_view, 16> QUESTION_WORDS{"?",    "？",   "とは", "なに", "何",   "なぜ",   "どう",       "必要",
                                                          "要素", "改善", "課題", "問題", "面白", "ゲーム", "プレイ", "ヴァンサバ"};
/** /要素|改善|課題|問題|面白|ゲーム|プレイ|ヴァンサバ|vampire/ */
constexpr std::array<std::string_view, 9> DESIGN_WORDS{"要素", "改善", "課題", "問題", "面白", "ゲーム", "プレイ", "ヴァンサバ", "vampire"};

template <std::size_t N>
bool contains_any(std::string_view text, const std::array<std::string_view, N>& words) {
    for (auto w : words)
        if (str::contains(text, w)) return true;
    return false;
}

std::string truncate(const std::string& value, std::size_t max) {
    if (str::js_length(value) <= max) return value;
    return str::js_slice(value, 0, static_cast<std::int64_t>(max) - 1) + "…";
}

/**
 * `raw.key?.trim()` — 無い / null は nullopt、文字列は trim。
 * 文字列以外は TS の TypeError ((x).trim is not a function) と同じく投げる。
 */
std::optional<std::string> opt_trim(const Json& raw, const char* key) {
    if (!raw.is_object()) return std::nullopt;
    const auto it = raw.find(key);
    if (it == raw.end() || it->is_null()) return std::nullopt;
    if (!it->is_string()) throw std::runtime_error(std::string("raw.") + key + ".trim is not a function");
    return str::trim(it->get<std::string>());
}

/** `x?.trim() || undefined` */
std::optional<std::string> nonempty(std::optional<std::string> v) {
    if (v && v->empty()) return std::nullopt;
    return v;
}

AutoDiscussionCategory normalize_category(const Json& raw) {
    if (raw.is_object()) {
        const auto it = raw.find("category");
        if (it != raw.end() && it->is_string()) {
            const auto c = it->get<std::string>();
            if (c == "game_design_question" || c == "mechanic_question" || c == "opinion" || c == "noise" || c == "command_like") return c;
        }
    }
    return "noise";
}

/**
 * enforce_game_title: LLM 判定時のみ true。LLM はゲーム名を本文から特定できるので、特定できない=議題が
 * 噛み合わないと判断し record_only に降格する。fallback はゲーム名を抽出できないので降格しない。
 */
AutoDiscussionClassification normalize_classification(const Json& raw, const std::string& content, const std::optional<std::string>& thread_title,
                                                      bool enforce_game_title = false) {
    AutoDiscussionClassification out;
    out.category = normalize_category(raw);
    const std::string trimmed_content = str::trim(content);
    out.title = truncate(nonempty(opt_trim(raw, "title")).value_or("Discord議題: " + trimmed_content), 80);
    out.description = truncate(nonempty(opt_trim(raw, "description")).value_or("Discord投稿から自動検出: " + trimmed_content), 600);
    // スレッドタイトルは対象ゲームの最優先根拠。LLM が gameTitle を返さなければ title を採用。
    const std::optional<std::string> title0 = thread_title ? std::optional<std::string>(str::trim(*thread_title)) : std::nullopt;
    out.gameTitle = nonempty(opt_trim(raw, "gameTitle"));
    if (!out.gameTitle) out.gameTitle = nonempty(title0);
    out.genre = nonempty(opt_trim(raw, "genre"));
    // 対象ゲームが特定できない議題は議論を開始しない (record_only に降格)。
    // ただし threadTitle が非空ならそれを主題アンカーに使えるので降格しない。
    const bool wants_start = raw.is_object() && raw.contains("action") && raw["action"].is_string() &&
                             raw["action"].get<std::string>() == "start_discussion";
    const bool downgrade = enforce_game_title && wants_start && !out.gameTitle && !(title0 && !title0->empty());
    out.action = wants_start && !downgrade ? "start_discussion" : "record_only";
    const auto reason = opt_trim(raw, "reason");
    out.reason = downgrade ? std::optional<std::string>("対象ゲームを特定できないため議論を開始しない (" + reason.value_or("gameTitle 不明") + ")")
                           : nonempty(reason);
    out.expectedAffect = nonempty(opt_trim(raw, "expectedAffect"));
    out.observedAffect = nonempty(opt_trim(raw, "observedAffect"));
    // TS の返り値の項目順 (action, category, title, description, gameTitle, genre, expectedAffect, observedAffect, reason)。
    return out;
}

std::optional<std::string> find_gap_by_evidence_utterance(core::Core& core, const std::string& workspace_id, const std::string& utterance_id) {
    const auto rows = core.raw().all("SELECT id, evidence_json FROM design_gaps WHERE workspace_id = ? AND evidence_json IS NOT NULL",
                                     {workspace_id}, kGapRowLimits);
    for (const auto& row : rows) {
        if (row.is_null("evidence_json")) continue;
        const std::string text = row.str("evidence_json");
        if (text.empty()) continue;
        const auto evidence = parse_json(text, kEvidenceJsonLimits);
        if (!evidence || !evidence->is_object()) continue;
        const auto it = evidence->find("utteranceIds");
        if (it == evidence->end() || !it->is_array()) continue;
        for (const auto& u : *it)
            if (u.is_string() && u.get<std::string>() == utterance_id) return row.str("id");
    }
    return std::nullopt;
}

} // namespace

std::string forum_direction_directive(const ForumDirection& direction) {
    return direction == "improvement"
               ? "【議論の方向性: 改善提案】この議題はゲーム/メカニクスの課題を洗い出し、具体的な改善案を出し合って収束させる。"
               : "【議論の方向性: 面白さ】この議題は何がどう面白いのか、体験の魅力を多角的に語り合って収束させる。";
}

Json to_json(const AutoDiscussionClassification& c) {
    Json j = Json::object();
    j["action"] = c.action;
    j["category"] = c.category;
    j["title"] = c.title;
    j["description"] = c.description;
    if (c.gameTitle) j["gameTitle"] = *c.gameTitle;
    if (c.genre) j["genre"] = *c.genre;
    if (c.expectedAffect) j["expectedAffect"] = *c.expectedAffect;
    if (c.observedAffect) j["observedAffect"] = *c.observedAffect;
    if (c.reason) j["reason"] = *c.reason;
    return j;
}

std::optional<Json> parse_classification_json(std::string_view text) {
    if (text.size() > kMaxClassificationTextBytes) return std::nullopt;
    const std::string trimmed = str::trim(text);
    const auto open = trimmed.find('{');
    const auto close = trimmed.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close < open) return std::nullopt;
    auto parsed = parse_json(std::string_view(trimmed).substr(open, close - open + 1), kClassificationJsonLimits);
    if (!parsed || !(parsed->is_object() || parsed->is_array())) return std::nullopt;
    return parsed;
}

AutoDiscussionClassification classify_discord_message_fallback(const std::string& content, const std::optional<std::string>& thread_title) {
    const std::string text = str::trim(content);
    const std::string lower = str::to_lower(text);
    const bool is_question = contains_any(text, QUESTION_WORDS) || str::contains(text, "vampire");
    if (str::starts_with(text, "/") || str::starts_with(lower, "http")) {
        return normalize_classification(
            Json{{"action", "record_only"}, {"category", "command_like"}, {"title", ""}, {"description", ""}, {"reason", "command-like or link-only content"}},
            text, thread_title);
    }
    if (!is_question || str::js_length(text) < 6) {
        return normalize_classification(
            Json{{"action", "record_only"}, {"category", "noise"}, {"title", ""}, {"description", ""}, {"reason", "not enough discussion signal"}}, text,
            thread_title);
    }
    // フォールバックでも gameTitle はスレッドタイトルから採る (正規化側で threadTitle を採用)。
    return normalize_classification(Json{{"action", "start_discussion"},
                                         {"category", contains_any(text, DESIGN_WORDS) ? "game_design_question" : "mechanic_question"},
                                         {"title", "Discord議題: " + text},
                                         {"description", "Discord投稿から自動検出: " + text},
                                         {"expectedAffect", "unknown"},
                                         {"observedAffect", "question"},
                                         {"reason", "question or game-design terms detected"}},
                                    text, thread_title);
}

AutoDiscussionClassification classify_discord_message(const std::string& content, const pe::LLMClientPtr& llm,
                                                      const std::optional<std::string>& thread_title) {
    const std::optional<std::string> title = thread_title ? std::optional<std::string>(str::trim(*thread_title)) : std::nullopt;
    const auto fallback = classify_discord_message_fallback(content, title);
    if (!llm) return fallback;

    std::vector<std::string> lines{
        "Classify the post.",
        // スレッドタイトルは議論の主題 (対象ゲーム等)。本文に主題が無くてもこれを基準にする。
        (title && !title->empty()) ? "Thread title (議論の主題・対象ゲーム — これが対象ゲームの最優先根拠): " + *title : std::string(),
        "",
        "【ゲームタイトル・ジャンルの判定 (最重要)】",
        "- 投稿本文(とスレッドタイトル)から、議論の対象となる『ゲームタイトル』と『ジャンル』を特定する。",
        "- gameTitle: 具体的なゲーム名 (例「Vampire Survivors」「ワンダと巨像」)。特定できなければ空文字。",
        "- genre: そのゲームのジャンル (例「ローグライト」「アクションアドベンチャー」「ヴァンサバ系」)。",
        "- 整合性を判断する: genre がその gameTitle に実際に合っているか? 投稿が本当にその対象についてか?",
        "  矛盾する / どのゲーム・ジャンルか特定できない 場合は action=record_only にし、reason に理由を書く。",
        "  (対象ゲーム・ジャンルが定まらない議題は噛み合わないので議論を開始しない。)",
        "- start_discussion にするのは、gameTitle が特定でき、genre と整合し、議論に値する内容のときだけ。",
        "",
        "JSON schema:",
        R"JSON({"action":"start_discussion|record_only","category":"game_design_question|mechanic_question|opinion|noise|command_like","gameTitle":"対象ゲーム名 or 空","genre":"ジャンル or 空","title":"short Japanese title","description":"本文の要点を踏まえた日本語要約","expectedAffect":"optional","observedAffect":"optional","reason":"short reason (特に開始しない場合は理由)"})JSON",
        "",
        "Post: " + content,
    };
    std::vector<std::string> kept;
    for (auto& l : lines)
        if (!l.empty()) kept.push_back(std::move(l));

    pe::LLMInvokeArgs args;
    args.system =
        "You classify Discord posts for an automatic game-design discussion system. "
        "You MUST read the post body carefully and identify which game and genre it is about. Return only compact JSON.";
    args.prompt = str::join(kept, "\n");
    args.maxTokens = 700;
    const auto result = llm->invoke(args);
    if (!result.ok) return fallback;
    const auto parsed = parse_classification_json(result.text);
    return normalize_classification(parsed ? *parsed : to_json(fallback), content, title, true);
}

StartAutoDiscussionResult start_auto_discussion_for_discord_message(core::Core& core, const DiscordAutoDiscussionInput& input,
                                                                    const pe::LLMClientPtr& llm) {
    const auto existing = find_gap_by_evidence_utterance(core, input.workspaceId, input.utteranceId);
    const auto classification = classify_discord_message(input.content, llm, input.forumTitle);
    if (existing) return {false, existing, classification};
    if (classification.action != "start_discussion") return {false, std::nullopt, classification};

    // 議題のアンカー: 対象ゲーム + ジャンル を gap タイトル/説明の先頭に固定する。
    // これが persona に渡る (ContextGap.description) ことで、議論が対象ゲームから逸れない。
    const std::optional<std::string> forum_title = input.forumTitle ? std::optional<std::string>(str::trim(*input.forumTitle)) : std::nullopt;
    const bool has_forum_title = forum_title && !forum_title->empty();
    std::optional<std::string> game_title = classification.gameTitle ? nonempty(str::trim(*classification.gameTitle)) : std::nullopt;
    if (!game_title && has_forum_title) game_title = forum_title;
    const std::optional<std::string> genre = classification.genre ? std::optional<std::string>(str::trim(*classification.genre)) : std::nullopt;
    const std::string gap_title = game_title ? *game_title : (has_forum_title ? *forum_title : classification.title);
    // 主題アンカー (ゲーム + ジャンル) → 投稿の要点 の順で description を組む。
    std::string anchor_line;
    if (game_title) {
        anchor_line = "【対象ゲーム: " + *game_title + (genre && !genre->empty() ? " / ジャンル: " + *genre : std::string()) + "】" +
                      "この議論は必ずこのゲーム・ジャンルに即して論じる。別ゲームの話や前提のすり替えは持ち込まない。";
    } else if (has_forum_title) {
        anchor_line = "【主題: " + *forum_title + "】";
    }
    std::vector<std::string> parts;
    if (!anchor_line.empty()) parts.push_back(anchor_line);
    if (!classification.description.empty()) parts.push_back(classification.description);
    const std::string base_description = str::join(parts, "\n");

    // 方向性 (フォーラムタグ由来) があれば議題説明にディレクティブを差し込む。
    const std::string description =
        input.direction && !input.direction->empty() ? base_description + "\n\n" + forum_direction_directive(*input.direction) : base_description;

    core::repositories::DesignGapInput gap;
    gap.workspaceId = input.workspaceId;
    gap.title = gap_title;
    gap.description = description;
    gap.status = "open";
    const std::string gap_id = core.repos.designGap.create(gap);

    // 適用タグ名 (前後空白除去 + 空要素除去)。結論まとめのフィルタ用。
    Json tags = Json::array();
    if (input.forumTags)
        for (const auto& t : *input.forumTags) {
            auto s = str::trim(t);
            if (!s.empty()) tags.push_back(std::move(s));
        }
    Json evidence = Json::object();
    evidence["source"] = "discord:auto-classifier";
    evidence["category"] = classification.category;
    evidence["gameTitle"] = game_title ? Json(*game_title) : Json(nullptr);
    evidence["genre"] = genre ? Json(*genre) : Json(nullptr);
    evidence["tags"] = tags;
    evidence["reason"] = classification.reason ? Json(*classification.reason) : Json(nullptr);
    evidence["direction"] = input.direction ? Json(*input.direction) : Json(nullptr);
    evidence["utteranceIds"] = Json::array({input.utteranceId});
    evidence["guildId"] = input.guildId;
    evidence["channelId"] = input.channelId;
    evidence["sessionId"] = input.sessionId;
    evidence["authorId"] = input.authorId;
    core.raw().run(
        "UPDATE design_gaps\n"
        "       SET gap_in = ?, expected_affect = ?, observed_affect = ?, evidence_json = ?, updated_at = ?\n"
        "       WHERE id = ?",
        // gap_in は dedup ユニーク索引 (workspace_id, gap_in, expected_affect, observed_affect) のキー。
        // discord 議論はスレッド/投稿ごとに独立なので gapId を含めて一意化する。
        {"discord:" + classification.category + ":" + gap_id, classification.expectedAffect, classification.observedAffect,
         json_stringify(evidence), time::now_ms(), gap_id});

    return {true, gap_id, classification};
}

std::function<StarterResult(const DiscordAutoDiscussionInput&)> create_discord_auto_discussion_starter(DiscordAutoDiscussionStarterOptions options) {
    return [options = std::move(options)](const DiscordAutoDiscussionInput& input) -> StarterResult {
        core::CoreOptions core_options;
        core_options.sqlitePath = core::resolve_active_kg_path(config::get_config());
        auto core = core::create_core(core_options);
        try {
            const auto r = start_auto_discussion_for_discord_message(*core, input, options.getLlm ? options.getLlm() : nullptr);
            core->close();
            // started=true は「議論の種 (designGap) が新規に立った = 開始エントリ」を意味する。
            // caller (gateway) はこの時だけリアクションを付ける。
            return {r.started};
        } catch (...) {
            core->close();
            throw;
        }
    };
}

} // namespace di::discord_hook
