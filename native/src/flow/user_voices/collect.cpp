#include "flow/user_voices/collect.hpp"

#include "base/strings.hpp"
#include "flow/user_voices/nfkc.hpp"
#include "flow/user_voices/voluptas_impressions.hpp"

namespace di::flow::user_voices {

namespace cs = crawler::sources;

Json to_json(const GameVoiceReport& r) {
    Json j{{"title", r.title}, {"collected", r.collected}, {"imported", r.imported}, {"embedded", r.embedded}};
    if (r.unreleased) {
        j["unreleased"] = true;
        if (r.appId) j["appId"] = *r.appId;
        if (r.appName) j["appName"] = *r.appName;
    }
    j["channel"] = r.channel;
    j["gameSlug"] = r.gameSlug ? Json(*r.gameSlug) : Json(nullptr);
    if (!r.unreleased) {
        if (r.appId) j["appId"] = *r.appId;
        if (r.appName) j["appName"] = *r.appName;
    }
    if (r.voluptasDown) j["voluptasDown"] = *r.voluptasDown;
    return j;
}

std::string utterance_id_of(const cs::ExternalUtterance& item) { return "ext:" + item.source + ":" + item.nativeId; }

std::string steam_game_slug(std::int64_t app_id) { return "steam-" + std::to_string(app_id); }

std::string glab_game_slug(const std::string& title) {
    const std::string lowered = js_to_lower(nfkc_approx(title));
    // .replace(/\s+/g, "-")
    std::string dashed;
    bool in_space = false;
    std::size_t i = 0;
    while (i < lowered.size()) {
        const std::size_t start = i;
        const std::uint32_t cp = str::next_codepoint(lowered, i);
        if (str::is_js_space(cp)) {
            if (!in_space) dashed += '-';
            in_space = true;
        } else {
            in_space = false;
            dashed.append(lowered, start, i - start);
        }
    }
    const std::string norm = str::js_slice(dashed, 0, 48);
    return "glab-" + (norm.empty() ? std::string("game") : norm);
}

namespace {

void call(const std::function<void(const std::string&)>& fn, const std::string& m) {
    if (fn) fn(m);
}

struct Counts {
    int imported = 0;
    int embedded = 0;
};

Counts import_and_embed(const std::vector<cs::ExternalUtterance>& items, const CollectUserVoicesDeps& deps) {
    if (items.empty()) return {};
    Counts c;
    c.imported = deps.importItems ? deps.importItems(deps.core, items, deps.workspaceId) : 0;
    if (deps.embed) {
        try {
            // 再取込 (dedup で skip) された声も、ベクトルが無ければ埋める。
            std::vector<std::string> ids;
            ids.reserve(items.size());
            for (const auto& it : items) ids.push_back(utterance_id_of(it));
            c.embedded = deps.embed(deps.core, ids);
        } catch (const std::exception& e) {
            call(deps.warn, std::string("ユーザーの声のベクトル化に失敗 (キーワード検索で継続): ") + e.what());
        }
    }
    return c;
}

} // namespace

GameVoiceReport collect_game_voices(const GameRef& game, const CollectUserVoicesDeps& deps) {
    GameVoiceReport base;
    base.title = game.title;
    const int existing = deps.existingCount ? deps.existingCount(game) : 0;
    if (existing > 0) {
        GameVoiceReport r = base;
        r.channel = "di";
        r.collected = existing;
        return r;
    }
    std::optional<SteamApp> app;
    try {
        if (deps.resolveSteam) app = deps.resolveSteam(game);
    } catch (const std::exception& e) {
        call(deps.warn, "Steam 解決に失敗 (" + game.title + "): " + e.what());
    }

    if (app && app->released) {
        const std::string slug = steam_game_slug(app->appId);
        try {
            const auto items = deps.fetchSteam ? deps.fetchSteam(*app, slug) : std::vector<cs::ExternalUtterance>{};
            const Counts c = import_and_embed(items, deps);
            call(deps.log, "Steam「" + app->name + "」(" + std::to_string(app->appId) + "): " + std::to_string(items.size()) + " 件取得 → 取込 " +
                               std::to_string(c.imported) + " / ベクトル化 " + std::to_string(c.embedded));
            if (!items.empty()) {
                GameVoiceReport r = base;
                r.channel = "steam";
                r.gameSlug = slug;
                r.appId = app->appId;
                r.appName = app->name;
                r.collected = static_cast<int>(items.size());
                r.imported = c.imported;
                r.embedded = c.embedded;
                return r;
            }
        } catch (const std::exception& e) {
            call(deps.warn, "Steam レビュー取得に失敗 (" + app->name + "): " + e.what());
        }
    }

    if (deps.fetchYoutube) {
        try {
            const std::string slug = "youtube-" + glab_game_slug(game.title);
            const auto items = deps.fetchYoutube(game, slug);
            if (!items.empty()) {
                const Counts c = import_and_embed(items, deps);
                GameVoiceReport r = base;
                r.imported = c.imported;
                r.embedded = c.embedded;
                r.channel = "youtube";
                r.gameSlug = slug;
                r.collected = static_cast<int>(items.size());
                return r;
            }
        } catch (const std::exception& e) {
            call(deps.warn, "YouTube 取得に失敗 (" + game.title + "): " + e.what());
        }
    }

    GameVoiceReport with_unreleased = base;
    if (app && !app->released) {
        with_unreleased.unreleased = true;
        with_unreleased.appId = app->appId;
        with_unreleased.appName = app->name;
    }
    const std::string slug = glab_game_slug(game.title);
    // Voluptas が自動で届けた感想 (KG 保存済み)。取りに行けないときの拠り所。
    auto stored = [&]() -> std::optional<GameVoiceReport> {
        const int n = deps.storedGlabCount ? deps.storedGlabCount(slug) : 0;
        if (n <= 0) return std::nullopt;
        call(deps.log, "Voluptas「" + game.title + "」: 届いていた感想 " + std::to_string(n) + " 件を使う");
        GameVoiceReport r = with_unreleased;
        r.channel = "glab";
        r.gameSlug = slug;
        r.collected = n;
        return r;
    };
    auto none = [&](std::optional<bool> down) {
        GameVoiceReport r = with_unreleased;
        r.channel = "none";
        r.gameSlug.reset();
        r.voluptasDown = down;
        return r;
    };
    if (!deps.fetchGlab) {
        if (auto s = stored()) return *s;
        return none(std::nullopt);
    }
    try {
        const auto items = deps.fetchGlab(game, slug);
        if (items.empty()) {
            if (auto s = stored()) return *s;
            return none(std::nullopt);
        }
        const Counts c = import_and_embed(items, deps);
        call(deps.log, "Voluptas「" + game.title + "」: 感想 " + std::to_string(items.size()) + " 件取得 → 取込 " + std::to_string(c.imported) +
                           " / ベクトル化 " + std::to_string(c.embedded));
        GameVoiceReport r = with_unreleased;
        r.channel = "glab";
        r.gameSlug = slug;
        r.collected = static_cast<int>(items.size());
        r.imported = c.imported;
        r.embedded = c.embedded;
        return r;
    } catch (const VoluptasUnavailable& e) {
        call(deps.warn, "Voluptas の感想取得に失敗 (" + game.title + "): " + e.what());
        if (auto s = stored()) return *s;
        return none(true);
    } catch (const std::exception& e) {
        call(deps.warn, "Voluptas の感想取得に失敗 (" + game.title + "): " + e.what());
        if (auto s = stored()) return *s;
        return none(false);
    }
}

std::vector<GameVoiceReport> collect_user_voices(const std::vector<GameRef>& games, const CollectUserVoicesDeps& deps) {
    std::vector<GameVoiceReport> reports;
    for (const auto& g : games) reports.push_back(collect_game_voices(g, deps));
    return reports;
}

std::string describe_voice_report(const GameVoiceReport& r) {
    if (r.voluptasDown.value_or(false)) return "「" + r.title + "」: Voluptas（Vo）が停止中、または接続できません。外部の声なしでも議論できます。";
    if (r.channel == "di") return "「" + r.title + "」: Di の収集済みデータ " + std::to_string(r.collected) + " 件を参考として使用";
    if (r.channel == "youtube") return "「" + r.title + "」: YouTube コメント " + std::to_string(r.collected) + " 件を参考として使用";
    const std::string name = r.appName ? *r.appName : r.title;
    if (r.channel == "steam") {
        return "「" + name + "」: Steam レビュー " + std::to_string(r.collected) + " 件 (取込 " + std::to_string(r.imported) + " / ベクトル化 " +
               std::to_string(r.embedded) + ")";
    }
    const std::string prefix = r.unreleased ? "「" + name + "」は Steam で未リリース。" : "「" + r.title + "」は Steam で見つからず。";
    if (r.channel == "glab") {
        return prefix + "Voluptas の遊んだ感想 " + std::to_string(r.collected) + " 件 (取込 " + std::to_string(r.imported) + " / ベクトル化 " +
               std::to_string(r.embedded) + ")";
    }
    return "「" + r.title + "」: 今回の取得先から外部の声を取得できませんでした。0 件でも議論できます（通信失敗・未設定の場合も含みます）";
}

} // namespace di::flow::user_voices
