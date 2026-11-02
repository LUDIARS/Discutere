#pragma once
// DiscussionContextProvider — persona-engine が議論データを読み書きする境界 (src/persona-engine/context-provider.ts)。
// consumer (Discatier 等) がこの純粋仮想クラスを実装する。persona-engine 内部は Discutere の具体型を参照しない。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::persona_engine {

struct ContextHypothesis {
    std::string id;
    std::string statement;
    std::string status;
    std::optional<std::string> designGapId;
    std::int64_t updatedAt = 0;
};

struct ContextGap {
    std::string id;
    std::string title;
    /** 議題の説明文 (対象ゲーム/ジャンルのアンカー + 投稿の要点)。JSON では null で出す。 */
    std::optional<std::string> description;
    std::string status;
    std::optional<std::string> gapIn;
    std::optional<std::string> expectedAffect;
    std::optional<std::string> observedAffect;
    std::vector<std::string> hypothesisIds;
    std::int64_t updatedAt = 0;
};

struct ContextUtterance {
    std::string id;
    std::string sessionId;
    std::string rawContent;
    std::int64_t postedAt = 0;
};

/**
 * 議題に関連する「外部の生の声」(RAG で prompt に注入する素材、§14)。
 * 個人はペルソナ名へマスク済 (speakerLabel)、出所 (source/sourceUrl) は開示する (§6)。
 */
struct ContextExternalVoice {
    std::string id;
    std::string content;
    /** ソース種別 (youtube / niconico / steam / ...)。露出可。 */
    std::string source;
    /** 元 URL (attribution があれば)。露出可。 */
    std::optional<std::string> sourceUrl;
    /** 露出用の仮名ラベル (例 `論者#a1b2`)。公開 ID は出さない。 */
    std::string speakerLabel;
    /** キーワード一致 + opinion-score の合成スコア。 */
    double score = 0;
};

struct ProposeHypothesisInput {
    std::string workspaceId;
    std::string statement;
    std::optional<std::string> designGapId;
    std::string byPersonaId;
    std::optional<std::string> reasoning;
};

struct PostUtteranceInput {
    std::string workspaceId;
    std::string sessionId;
    std::string text;
    std::string byPersonaId;
    std::optional<std::string> respondsTo;
};

struct FindDiscussionSessionInput {
    std::string workspaceId;
    std::optional<std::string> hypothesisId;
    std::optional<std::string> gapId;
};

class DiscussionContextProvider {
public:
    virtual ~DiscussionContextProvider() = default;

    virtual std::vector<ContextHypothesis> list_active_hypotheses(const std::string& workspace_id, int limit) = 0;
    virtual std::vector<ContextGap> list_recent_gaps(const std::string& workspace_id, int limit) = 0;
    virtual std::vector<ContextUtterance> list_recent_utterances(const std::string& workspace_id, const std::string& session_id,
                                                                 int limit) = 0;

    /**
     * 議題語 (terms) に関連する外部の生の声を active KG から取得する (RAG / §14)。
     * 実装は任意 (既定は空 = prompt に外部の声は載らない、従来挙動)。
     */
    virtual std::vector<ContextExternalVoice> list_relevant_external_voices(const std::string& /*workspace_id*/,
                                                                            const std::vector<std::string>& /*terms*/,
                                                                            int /*limit*/) {
        return {};
    }

    /**
     * 指定 gap への「進行役への調整指示」を新しい順 limit 件、時系列 (古い→新しい) の本文配列で返す。
     * 実装は任意 (既定は空)。
     */
    virtual std::vector<std::string> list_facilitator_directives(const std::string& /*workspace_id*/,
                                                                 const std::string& /*gap_id*/, int /*limit*/) {
        return {};
    }

    virtual std::string propose_hypothesis(const ProposeHypothesisInput& input) = 0;
    virtual std::string post_utterance(const PostUtteranceInput& input) = 0;

    /** hypothesis / gap から紐付く議論 session を引く。無ければ nullopt (既定)。 */
    virtual std::optional<std::string> find_discussion_session(const FindDiscussionSessionInput& /*input*/) {
        return std::nullopt;
    }

    /** いま進行中の discord-bound 議論 session。無ければ nullopt (既定)。 */
    virtual std::optional<std::string> find_active_discussion_session(const std::string& /*workspace_id*/) {
        return std::nullopt;
    }
};

// ─── JSON (prompt に載せる議論コンテキスト。キー順は TS のオブジェクトリテラル順) ───
Json to_json(const ContextHypothesis& h);
Json to_json(const ContextGap& g);
Json to_json(const ContextUtterance& u);
Json to_json(const ContextExternalVoice& v);

} // namespace di::persona_engine
