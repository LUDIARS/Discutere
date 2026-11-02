#pragma once
// core のリポジトリ群 (src/core/repositories/base.ts)。
// 作成・更新はイベント (EventLog::append_event) として記録し、射影で索引表に反映する。
// games / aesthetics は TS と同じく索引表へ直接書く。get / list は TS と同じ SQL (SELECT *)。
//
// payload は TS の `{ ...input, id }` と同じ形: 省略した項目 (nullopt) は出さず、
// id は呼び出し側が渡したときは先頭、渡さなかったときは末尾に置く (TS では呼び出し側のオブジェクト
// リテラルの順になる。C++ は入力 struct の宣言順 = TS の型の宣言順)。
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "core/db/kuzu_client.hpp"
#include "core/events/event_log.hpp"

namespace di::core::repositories {

struct CoreContext {
    db::KuzuClient* client = nullptr;
    events::EventLog* eventLog = nullptr;
};

CoreContext make_context(db::KuzuClient& client, events::EventLog& event_log);

/** list の行数・バイト数の上限 (DB の値も外部入力として課金する)。 */
inline constexpr sql::QueryLimits kRepoListLimits{500'000, 256u << 20};

// ─── 入力 ────────────────────────────────────────────────────────
struct PersonInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::string name;
    std::optional<std::string> role;
};
struct PersonPatch {
    std::string workspaceId;
    std::string name;
    std::optional<std::string> role;
};
struct GameInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::string title;
    std::optional<std::string> genre;
};
struct GamePatch {
    std::string workspaceId;
    std::string title;
    std::optional<std::string> genre;
};
struct MechanicInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::optional<std::string> gameId;
    std::string name;
    std::optional<std::string> description;
};
struct MechanicPatch {
    std::string workspaceId;
    std::optional<std::string> gameId;
    std::string name;
    std::optional<std::string> description;
};
struct AestheticInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::optional<std::string> gameId;
    std::string name;
    std::optional<std::string> description;
};
struct AestheticPatch {
    std::string name;
    std::optional<std::string> description;
};
struct AffectInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::optional<std::string> sessionId;
    std::optional<std::string> subjectId;
    std::string mood;
    std::optional<double> score;
};
struct AffectPatch {
    std::string workspaceId;
    std::optional<std::string> sessionId;
    std::optional<std::string> subjectId;
    std::string mood;
    std::optional<double> score;
};
struct PlayContextInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::optional<std::string> gameId;
    std::optional<std::string> platform;
    std::optional<std::string> mode;
};
struct PlayContextPatch {
    std::string workspaceId;
    std::optional<std::string> gameId;
    std::optional<std::string> platform;
    std::optional<std::string> mode;
};
struct SessionInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::string title;
    double startedAt = 0;
    std::optional<double> endedAt;
    std::optional<std::string> mode;
    std::optional<std::string> scene;
};
struct SessionPatch {
    std::string workspaceId;
    std::string title;
    double startedAt = 0;
    std::optional<double> endedAt;
    std::optional<std::string> mode;
    std::optional<std::string> scene;
};
struct UtteranceInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::string sessionId;
    std::optional<std::string> speakerId;
    std::string rawContent;
    std::optional<std::string> normalizedContent;
    double postedAt = 0;
    std::optional<std::string> respondsTo;
};
struct UtterancePatch {
    std::string workspaceId;
    std::string normalizedContent;
    std::optional<std::string> rawContent;
};
struct ReactionInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::string utteranceId;
    std::optional<std::string> actorId;
    std::string reactionType;
    std::optional<double> intensity;
};
struct ReactionPatch {
    std::string workspaceId;
    std::string utteranceId;
    std::optional<std::string> actorId;
    std::string reactionType;
    std::optional<double> intensity;
};
struct DesignGapInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::optional<std::string> gameId;
    std::string title;
    std::optional<std::string> description;
    std::optional<std::string> status;
};
struct DesignGapPatch {
    std::string workspaceId;
    std::optional<std::string> gameId;
    std::string title;
    std::optional<std::string> description;
    std::optional<std::string> status;
};
struct HypothesisInput {
    std::optional<std::string> id;
    std::string workspaceId;
    std::optional<std::string> designGapId;
    std::string statement;
    std::optional<std::string> status;
    std::optional<bool> integrated;
};
struct HypothesisPatch {
    std::string workspaceId;
    std::optional<std::string> designGapId;
    std::string statement;
    std::optional<std::string> status;
    std::optional<bool> integrated;
};

/** 索引表 1 つぶんの読み出し (get / list の共通部分)。 */
class TableReader {
public:
    TableReader(CoreContext ctx, std::string table) : ctx_(ctx), table_(std::move(table)) {}
    std::optional<sql::Row> get(const std::string& id) const;
    std::vector<sql::Row> list(const std::string& workspace_id) const;

protected:
    CoreContext ctx_;
    std::string table_;
};

class PersonRepo : public TableReader {
public:
    explicit PersonRepo(CoreContext ctx) : TableReader(ctx, "people") {}
    std::string create(const PersonInput& input) const;
    void update(const std::string& id, const PersonPatch& patch) const;
};
class GameRepo : public TableReader {
public:
    explicit GameRepo(CoreContext ctx) : TableReader(ctx, "games") {}
    std::string create(const GameInput& input) const;
    void update(const std::string& id, const GamePatch& patch) const;
};
class MechanicRepo : public TableReader {
public:
    explicit MechanicRepo(CoreContext ctx) : TableReader(ctx, "mechanics") {}
    std::string create(const MechanicInput& input) const;
    void update(const std::string& id, const MechanicPatch& patch) const;
};
class AestheticRepo : public TableReader {
public:
    explicit AestheticRepo(CoreContext ctx) : TableReader(ctx, "aesthetics") {}
    std::string create(const AestheticInput& input) const;
    void update(const std::string& id, const AestheticPatch& patch) const;
};
class AffectRepo : public TableReader {
public:
    explicit AffectRepo(CoreContext ctx) : TableReader(ctx, "affects") {}
    std::string create(const AffectInput& input) const;
    void update(const std::string& id, const AffectPatch& patch) const;
};
class PlayContextRepo : public TableReader {
public:
    explicit PlayContextRepo(CoreContext ctx) : TableReader(ctx, "play_contexts") {}
    std::string create(const PlayContextInput& input) const;
    void update(const std::string& id, const PlayContextPatch& patch) const;
};
class SessionRepo : public TableReader {
public:
    explicit SessionRepo(CoreContext ctx) : TableReader(ctx, "sessions") {}
    std::string create(const SessionInput& input) const;
    void update(const std::string& id, const SessionPatch& patch) const;
};
class UtteranceRepo : public TableReader {
public:
    explicit UtteranceRepo(CoreContext ctx) : TableReader(ctx, "utterances") {}
    std::string create(const UtteranceInput& input) const;
    /** 無い発話は std::runtime_error("utterance not found")、raw_content の書き換えは
     *  std::runtime_error("Utterance.raw_content is immutable")。 */
    void update(const std::string& id, const UtterancePatch& patch) const;
};
class ReactionRepo : public TableReader {
public:
    explicit ReactionRepo(CoreContext ctx) : TableReader(ctx, "reactions") {}
    std::string create(const ReactionInput& input) const;
    void update(const std::string& id, const ReactionPatch& patch) const;
};
class DesignGapRepo : public TableReader {
public:
    explicit DesignGapRepo(CoreContext ctx) : TableReader(ctx, "design_gaps") {}
    std::string create(const DesignGapInput& input) const;
    void update(const std::string& id, const DesignGapPatch& patch) const;
};
class HypothesisRepo : public TableReader {
public:
    explicit HypothesisRepo(CoreContext ctx) : TableReader(ctx, "hypotheses") {}
    std::string create(const HypothesisInput& input) const;
    void update(const std::string& id, const HypothesisPatch& patch) const;
};

} // namespace di::core::repositories
