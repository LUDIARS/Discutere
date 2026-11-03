#pragma once
// src/flow/entry-discord.ts の移植: Discord フォーラム投稿を入口とするフロー起動 (t7-entrypoints.md 経路 B)。
//
// フォーラムの **投稿が UI**。最初の投稿 (starter) をトリガーにテーマを受信し、
// **議論タイプ (flow) とタグ (機密/内部/運用/開発) はいずれもフォーラムの適用タグから取得**する
// (slash / ボタンは使わない、OVERVIEW §9 の決定)。議論タイプは必須 (未指定は受理しない)。
//
// 認証は既存どおり Discord 依存 (bot token + admin allowlist、Cernere 非使用、CLAUDE.md)。
// 本モジュールは「フォーラム適用タグ → (flow, tags) → dispatch」の純パーサ + ハンドラのみを提供する。
// 正規表現は使わない (JS の /.../i と同じ一致を線形の走査で書く)。
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "flow/dispatch.hpp"
#include "flow/tags.hpp"

namespace di::flow {

/** フォーラム適用タグ名 → FlowKind / FlowTag のマッピング (config 上書き可)。 */
struct ForumTagMapping {
    /** 議論タイプ判定用のタグ名 (kind ごと、TS のオブジェクトの挿入順)。parse_flow_kind の別名と統合される。 */
    std::optional<std::vector<std::pair<FlowKind, std::vector<std::string>>>> flowKindTagNames;
    /** 機密度+観点タグの名前 (既定: 機密/内部/運用/開発/未来予測/代理 のリテラル)。 */
    std::optional<std::vector<std::string>> flowTagNames;
};

struct ParsedForumEntry {
    /** 解決した議論タイプ。タグから判定できなければ nullopt (= 受理しない)。 */
    std::optional<FlowKind> flow;
    /** 抽出した機密度+観点タグ。 */
    std::vector<FlowTag> tags;
};
Json to_json(const ParsedForumEntry& p);

/**
 * フォーラム適用タグ名から議論タイプ + フロータグを抽出する。
 * - 議論タイプ: タグ名を parse_flow_kind で解決 (+ override の別名)。複数該当時は最初の解決を採用。
 * - フロータグ: 機密/内部/運用/開発 等に一致する (または含む) 適用タグを抽出。
 */
ParsedForumEntry parse_forum_entry(const std::vector<std::string>& applied_tag_names, const std::optional<ForumTagMapping>& mapping = std::nullopt);

struct RoundsTurns {
    std::optional<double> rounds;
    std::optional<double> turnsPerRound;
};
Json to_json(const RoundsTurns& r);

/**
 * starter 本文から議論ごとのラウンド/ターン指定を抽出する (任意)。
 * 例: 「ラウンド:5 ターン:8」「rounds=3 turns=6」。見つからなければ nullopt。
 */
RoundsTurns parse_rounds_turns(std::string_view text);

/**
 * starter 本文から壁打ち相手指定を抽出する (G)。
 * 例: 「相手:ローグ好き太郎,ソシャゲ花子」「opponent=花子」。カンマ区切り。無ければ nullopt。
 */
std::optional<std::vector<std::string>> parse_opponents(std::string_view text);

/** フォーラム starter 投稿の最小入力 (gateway/forum-monitor 由来)。 */
struct ForumPostInput {
    /** starter 投稿本文 = テーマ。 */
    std::string theme;
    /** フォーラムの適用タグ名。 */
    std::vector<std::string> appliedTagNames;
    /** scene 文字列 (discord:<guild>/<thread>)。記録用。 */
    std::optional<std::string> scene;
};

/** { ok: true, dispatch } | { ok: false, reason: "flow-required", message } */
struct ForumEntryResult {
    bool ok = false;
    std::optional<DispatchResult> dispatch;
    std::string reason;
    std::string message;
};

/** 議論タイプが無いときの案内文 (TS と同じ文言)。 */
inline constexpr std::string_view FLOW_REQUIRED_MESSAGE =
    "議論タイプのタグが付いていません。「企画/議論」または「改善/議論」を選択してください。";

/**
 * フォーラム投稿を受けてフローを起動する (同期。呼んだスレッドで議論を最後まで進める)。
 * 議論タイプが適用タグから判定できなければ受理しない (ok=false, flow-required)。
 */
ForumEntryResult handle_forum_flow_post(const ForumPostInput& input, const DispatchDeps& deps,
                                        const std::optional<ForumTagMapping>& mapping = std::nullopt);

} // namespace di::flow
