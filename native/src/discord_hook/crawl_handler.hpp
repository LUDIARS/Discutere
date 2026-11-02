#pragma once
// crawl チャンネルのメッセージを取り込み処理に流す orchestrator (id=60 + id=61)。
// (src/discord-hook/crawl-handler.ts の移植)
//
// Gateway (後の担当) が crawl チャンネルの MESSAGE_CREATE を受けたらこの関数を呼ぶ。
// メッセージ本文から URL を取り出して取り込み、進捗をリアクションで示し、結果を「データ追加」通知チャンネルへ投稿する。
// 取り込みロジック自体は crawl_channel (transport 非依存) に委譲する。Discord の操作は discord_api (REST v10)。
// 取り込みは数十秒かかるので、Gateway のスレッドではなく background_executor から呼ぶこと。
#include <optional>
#include <string>

#include "discord_hook/crawl_channel.hpp"
#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

struct CrawlHandlerDeps {
    CrawlDeps crawl;
    /** 通知先の親カテゴリ名 (既定「システム」) */
    std::optional<std::string> categoryName;
    /** 通知先チャンネル名 (既定「データ追加」) */
    std::optional<std::string> channelName;
};

/**
 * crawl メッセージ 1 件を処理する。URL が無ければ何もしない (false)。
 * 取り込みを試みたら true。例外は内部で握り、⚠️ を付けて返す。
 */
bool handle_crawl_message(DiscordApi& api, const Message& msg, const CrawlHandlerDeps& deps);

} // namespace di::discord_hook
