#pragma once
// ユーザーの声の収集の入口 (src/flow/user-voices/runtime.ts の移植、spec/feature/flow/user-voices.md)。
//
// 議論/改善の準備時に呼ぶ: 議題のゲーム + 類似ゲームの声を集めて取り込み・ベクトル化し、
// 議論に渡す「ゲームごとに均等に混ぜた」外部の声検索を返す。既定の実装 (Steam / Voluptas /
// 取込 / 埋め込み) をここで束ね、収集ロジック本体 (collect) は注入境界のまま保つ。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp"
#include "core/index.hpp"
#include "core/vectors/embedder.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "flow/user_voices/collect.hpp"
#include "flow/voice_cache.hpp"

namespace di::flow::user_voices {

/** 「」『』で囲まれた語 (2〜40 文字。ゲーム名の明示)。 */
std::vector<std::string> quoted_terms(const std::string& text);
/** 議題のゲーム (議題全文を match 対象に、明示名 → 議題の語の順で Steam 検索)。 */
GameRef theme_game_ref(const std::string& theme);
/** 類似ゲーム (指定名そのもので検索・判定)。 */
GameRef similar_game_ref(const std::string& name);
/** 本文から類似ゲームの指定を取り出す (「類似ゲーム「A」「B」」など、ペーパー調整の類似ゲーム指定と同じ書き方)。 */
std::vector<std::string> parse_similar_games(const std::optional<std::string>& text);

/** config.embedding → 埋め込みクライアントの設定。 */
core::vectors::EmbeddingConfig embedding_config_of(const config::DiscutereConfig& cfg);

using LogFn = std::function<void(const std::string&)>;

/** 既定の収集依存 (Steam / Voluptas / 取込 / 埋め込み) を組む。 */
CollectUserVoicesDeps default_collect_deps(core::Core* core, const std::string& workspace_id, LogFn log, LogFn warn,
                                           const config::DiscutereConfig* config = nullptr);

struct PreparedUserVoices {
    std::vector<GameVoiceReport> reports;
    /**
     * 議論/ペーパーへ渡す外部の声検索。集めた声があればゲームごとに均等に混ぜる。
     * 無効化・Core 未設定・全滅のときは空 (呼び出し側は既存の検索を使う)。
     */
    ListExternalVoicesFn lookup;
};

struct PrepareUserVoicesArgs {
    std::string theme;
    std::optional<std::string> youtubeApiKey;
    /** 企画段階ではテーマ自体を既存ゲームとして検索しない (false)。 */
    std::optional<bool> includeThemeGame;
    std::vector<std::string> similarGames;
    std::function<std::unique_ptr<core::Core>()> openCore;
    std::string workspaceId;
    /** 既存の外部の声検索 (議題のゲームの声が集まらなかった時に使う)。 */
    ListExternalVoicesFn baseLookup;
    LogFn log;
    LogFn warn;
    /** テスト用: 収集依存の差し替え。 */
    std::function<CollectUserVoicesDeps(core::Core* core)> collectDeps;
    /** テスト用: 議題の埋め込み (既定は config.embedding の埋め込みクライアント)。 */
    std::shared_ptr<core::vectors::EmbeddingClient> embedder;
    /** テスト用: 設定 (既定 get_config())。 */
    const config::DiscutereConfig* config = nullptr;
    /** テスト用: 声の引き出しに使う attribution を開く (既定 open_attribution_store())。 */
    std::function<std::unique_ptr<crawler::sources::AttributionStore>()> openAttribution;
};

/** 議題のゲーム + 類似ゲームの声を集め、均等混合の検索を返す。 */
PreparedUserVoices prepare_user_voices(const PrepareUserVoicesArgs& args);

} // namespace di::flow::user_voices
