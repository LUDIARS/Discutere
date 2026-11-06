#include "flow/learning.hpp"

#include <stdexcept>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "crawler/sources/importer.hpp"

namespace di::flow {

namespace cs = crawler::sources;

Json to_json(const LearningFlowResult& r) {
    return Json{{"gameSlug", r.gameSlug},
                {"opinionsRecorded", r.opinionsRecorded},
                {"mechanicsRecorded", r.mechanicsRecorded},
                {"polarityBreakdown", r.polarityBreakdown},
                {"crawledImported", r.crawledImported},
                {"crawledBySource", r.crawledBySource}};
}

LearningFlowResult run_learning_flow(const std::string& game_title, const std::string& theme, const LearningFlowDeps& deps) {
    const std::string workspace_id = deps.workspaceId.value_or("knowledge");
    const std::string games_dir = deps.gamesDir.value_or("./data/games");
    const LogFn log = deps.log ? deps.log : LogFn([](const std::string& m) { di::log::info("[learning] " + m); });
    const LogFn warn = deps.warn ? deps.warn : LogFn([](const std::string& m) { di::log::warn("[learning/warn] " + m); });
    if (!deps.core) throw std::invalid_argument("run_learning_flow requires a core");
    core::Core& core = *deps.core;

    // derive_slug は ASCII 化に失敗しても元タイトル等にフォールバックするため非空 (日本語 OK)。
    const std::string slug = deps.slug.value_or(derive_slug(game_title));
    if (slug.empty()) throw std::runtime_error("slug を導出できません (title=\"" + game_title + "\")。deps.slug を明示してください。");
    log("学習フロー開始: \"" + game_title + "\" (slug=" + slug + ", テーマ=\"" + theme + "\")");

    LearningFlowResult result;
    result.gameSlug = slug;

    // ── メカニクス記録 ───────────────────────────────────────────────────────
    if (!deps.mechanics.empty()) {
        // 出所ラベル (08): クロール/学習取込経路のメカニクスは source="crawl" を付けて永続する (明示済みの source は尊重)。
        std::vector<GameMechanicEntry> labeled = deps.mechanics;
        for (auto& m : labeled)
            if (!m.source || m.source->empty()) m.source = "crawl";
        result.mechanicsRecorded = upsert_game_mechanics(UpsertGameMechanicsArgs{games_dir, slug, game_title, labeled});
        // Discatier Core mechanics にも記録 (name/description)
        for (const auto& m : deps.mechanics) {
            if (m.name.empty()) continue;
            try {
                core::repositories::MechanicInput in;
                in.workspaceId = workspace_id;
                in.gameId = slug;
                in.name = m.name;
                in.description = m.description;
                core.repos.mechanic.create(in);
            } catch (const std::exception& err) {
                warn("メカニクス Core 記録に失敗 (" + m.name + "): " + err.what());
            }
        }
        log("メカニクス " + std::to_string(result.mechanicsRecorded) + " 件を data/games/" + slug + ".md + Core に記録");
    }

    // ── 意見収集 + 極性付与 + KG 記録 ────────────────────────────────────────
    result.polarityBreakdown = Json{{"positive", 0}, {"negative", 0}, {"neutral", 0}};
    std::vector<cs::ExternalUtterance> items;
    for (std::size_t i = 0; i < deps.opinions.size(); ++i) {
        const auto& op = deps.opinions[i];
        const std::string text = str::trim(op.content);
        if (text.empty()) continue;

        crawler::sentiment::SentimentResult sentiment;
        try {
            sentiment = crawler::sentiment::cascade_sentiment(text, std::nullopt, deps.sentimentClients);
        } catch (const std::exception& err) {
            warn("感情カスケード失敗 (意見 " + std::to_string(i) + "): " + err.what());
            sentiment = crawler::sentiment::SentimentResult{"neutral", 0, 0, 0};
        }
        auto& slot = result.polarityBreakdown[sentiment.polarity];
        slot = (slot.is_number() ? slot.get<std::int64_t>() : 0) + 1;

        cs::ExternalUtterance u;
        u.source = "feedback";
        // 匿名 + 冪等: スレッド内連番。個人 ID は持たない。
        u.nativeId = "learning:" + slug + ":" + (op.postedAt ? js_number_to_string(*op.postedAt) : std::string("t")) + ":" + std::to_string(i);
        u.gameSlug = slug;
        u.threadKey = "learning:" + slug;
        u.content = text;
        u.postedAt = op.postedAt ? *op.postedAt : static_cast<double>(time::now_ms());
        u.authorId = "anon";  // 個人アンカーを持たない (CLAUDE.md 個人データ非保管)
        u.sourceUrl = "discutere://game-feedback/" + slug;
        // 極性を賛否シグナルに反映 (positive/negative のみ。中立はシグナルなし)
        if (sentiment.polarity == "positive") u.signal = cs::ExternalSignal{true, std::nullopt};
        else if (sentiment.polarity == "negative") u.signal = cs::ExternalSignal{false, std::nullopt};
        items.push_back(std::move(u));
    }

    if (!items.empty()) {
        cs::ExternalImportOptions opts;
        opts.workspaceId = workspace_id;
        const auto res = cs::import_external_utterances(core, items, opts);
        result.opinionsRecorded = static_cast<int>(res.utterances);
        log("意見 " + std::to_string(result.opinionsRecorded) + " 件を KG に記録 (skip " + js_number_to_string(res.skipped) + ")");
    }

    // ── 自動収集モード (① 類似ゲームの自動収集) ─────────────────────────────────
    if (deps.crawl && !deps.crawl->sources.empty()) {
        const CollectAndImportFn do_collect = deps.collectAndImportFn ? deps.collectAndImportFn : CollectAndImportFn(collect_and_import);
        const auto& crawl = *deps.crawl;
        const std::string q = crawl.query ? str::trim(*crawl.query) : std::string();
        const std::string crawl_theme = q.empty() ? game_title : q;
        for (const auto& source : crawl.sources) {
            AutoCrawlSpec spec{source, crawl_theme, std::nullopt, std::nullopt, std::nullopt};
            if (auto it = crawl.specBySource.find(source); it != crawl.specBySource.end()) {
                spec.appId = it->second.appId;
                spec.urls = it->second.urls;
                spec.subreddit = it->second.subreddit;
            }
            CollectAndImportDeps c;
            c.core = deps.core;
            c.theme = crawl_theme;
            c.slug = slug;
            c.workspaceId = workspace_id;
            c.spec = spec;
            c.maxItems = crawl.maxItems;
            c.youtubeApiKey = crawl.youtubeApiKey;
            c.log = log;
            c.warn = warn;
            try {
                const auto r = do_collect(c);
                result.crawledImported += r.imported;
                auto& slot = result.crawledBySource[source];
                slot = (slot.is_number() ? slot.get<std::int64_t>() : 0) + r.imported;
            } catch (const std::exception& err) {
                // 1 ソースの失敗は学習全体を止めない (graceful degrade)。
                warn("自動収集失敗 (source=" + source + "): " + err.what());
            }
        }
        log("自動収集 " + std::to_string(result.crawledImported) + " 件を KG に取込 (" + str::join(crawl.sources, "/") + ")");
    }

    return result;
}

} // namespace di::flow
