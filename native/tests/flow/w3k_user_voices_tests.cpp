// w3-K: ユーザーの声 (tests/flow/user-voices.test.ts の collect / listGameVoices / parseSimilarGames 部分、
// impression-bridge.test.ts の「届いていた感想への fallback」)、importer (tests/crawler/external/importer.test.ts、
// summary.test.ts の importer 部分)、voice-index (部分ベクトル化)、prepare_user_voices。
#include <filesystem>

#include "check.hpp"
#include "core/test_util.hpp"

#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/vectors/voice_index.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/importer.hpp"
#include "crawler/sources/ingested_store.hpp"
#include "crawler/sources/raw_store.hpp"
#include "flow/user_voices/collect.hpp"
#include "flow/user_voices/game_voices.hpp"
#include "flow/user_voices/runtime.hpp"
#include "flow/user_voices/voluptas_impressions.hpp"

using namespace di;
using namespace di::flow::user_voices;
namespace cs = di::crawler::sources;

namespace {

cs::ExternalUtterance utt(std::string source, std::string native_id, std::string slug, std::string thread, std::string content, double at,
                          std::string author, std::string url = "u") {
    cs::ExternalUtterance u;
    u.source = std::move(source);
    u.nativeId = std::move(native_id);
    u.gameSlug = std::move(slug);
    u.threadKey = std::move(thread);
    u.content = std::move(content);
    u.postedAt = at;
    u.authorId = std::move(author);
    u.sourceUrl = std::move(url);
    return u;
}

/** 呼ばれた id を覚える偽の埋め込み (2 次元)。 */
class FakeEmbedder : public core::vectors::EmbeddingClient {
public:
    std::vector<std::vector<std::string>> batches;
    bool short_reply = false;
    std::vector<std::vector<double>> embed(const std::vector<std::string>& texts) override {
        batches.push_back(texts);
        std::vector<std::vector<double>> out;
        for (std::size_t i = 0; i < texts.size(); ++i) out.push_back({1.0, static_cast<double>(i)});
        if (short_reply && !out.empty()) out.pop_back();
        return out;
    }
};

CollectUserVoicesDeps base_deps(std::vector<std::vector<std::string>>* embedded) {
    CollectUserVoicesDeps d;
    d.workspaceId = "knowledge";
    d.fetchSteam = [](const SteamApp&, const std::string& slug) {
        return std::vector<cs::ExternalUtterance>{utt("steam", "r1", slug, "1", "面白い", 1, "S")};
    };
    d.fetchGlab = [](const GameRef&, const std::string& slug) {
        return std::vector<cs::ExternalUtterance>{utt("glab", "v1", slug, "新作", "重い", 1, "anonymous", "g")};
    };
    d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>& items, const std::string&) { return static_cast<int>(items.size()); };
    d.embed = [embedded](core::Core*, const std::vector<std::string>& ids) {
        embedded->push_back(ids);
        return static_cast<int>(ids.size());
    };
    return d;
}

} // namespace

// ── golden ───────────────────────────────────────────────────────────────

TEST_CASE("golden: theme_game_ref / parse_similar_games / describe_voice_report") {
    for (const auto& c : test::load_fixture("w3k/theme_game_refs.json")) {
        const auto ref = theme_game_ref(c["theme"].get<std::string>());
        Json terms = Json::array();
        for (const auto& t : ref.searchTerms) terms.push_back(t);
        CHECK_EQ(json_stringify(terms), json_stringify(c["ref"]["searchTerms"]));
        CHECK_EQ(ref.title, c["ref"]["title"].get<std::string>());
        CHECK_EQ(ref.matchAgainst, c["ref"]["matchAgainst"].get<std::string>());
    }
    for (const auto& c : test::load_fixture("w3k/similar_games.json")) {
        Json out = Json::array();
        for (const auto& t : parse_similar_games(c["text"].get<std::string>())) out.push_back(t);
        CHECK_EQ(json_stringify(out), json_stringify(c["out"]));
    }
    CHECK(parse_similar_games(std::nullopt).empty());
    for (const auto& c : test::load_fixture("w3k/voice_reports.json")) {
        const Json& j = c["report"];
        GameVoiceReport r;
        r.title = j["title"].get<std::string>();
        r.channel = j["channel"].get<std::string>();
        if (j["gameSlug"].is_string()) r.gameSlug = j["gameSlug"].get<std::string>();
        if (j.contains("appId")) r.appId = j["appId"].get<std::int64_t>();
        if (j.contains("appName")) r.appName = j["appName"].get<std::string>();
        r.unreleased = j.contains("unreleased") && j["unreleased"].get<bool>();
        r.collected = j["collected"].get<int>();
        r.imported = j["imported"].get<int>();
        r.embedded = j["embedded"].get<int>();
        if (j.contains("voluptasDown")) r.voluptasDown = j["voluptasDown"].get<bool>();
        CHECK_EQ(describe_voice_report(r), c["text"].get<std::string>());
        // to_json のキー順も TS の spread と同じ
        CHECK_EQ(json_stringify(to_json(r)), json_stringify(j));
    }
}

TEST_CASE("quoted_terms: 2〜40 文字の境界・入れ子・閉じ忘れ") {
    CHECK((quoted_terms("「ab」") == std::vector<std::string>{"ab"}));
    CHECK(quoted_terms("「a」").empty());
    CHECK((quoted_terms("「" + str::repeat("あ", 40) + "」") == std::vector<std::string>{str::repeat("あ", 40)}));
    CHECK(quoted_terms("「" + str::repeat("あ", 41) + "」").empty());
    CHECK((quoted_terms("「「ab」") == std::vector<std::string>{"「ab"}));
    CHECK(quoted_terms("「閉じない").empty());
    CHECK((quoted_terms("『ab」と「cd』") == std::vector<std::string>{"ab", "cd"}));
    CHECK((quoted_terms(str::repeat("「", 100000) + "x") .empty()));
}

// ── collect (user-voices.test.ts / impression-bridge.test.ts) ─────────────

TEST_CASE("collect: Steam / Di / YouTube / Voluptas / none の分岐 + ベクトル化 + 失敗時の継続") {
    std::vector<std::vector<std::string>> embedded;
    const GameRef game = theme_game_ref("「新作」の操作感");
    CHECK_EQ(game.searchTerms[0], std::string("新作"));

    auto d = base_deps(&embedded);
    d.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(SteamApp{7, "新作", true}); };
    const auto released = collect_game_voices(game, d);
    CHECK_EQ(released.channel, std::string("steam"));
    CHECK_EQ(released.gameSlug.value_or(""), steam_game_slug(7));
    CHECK((embedded.back() == std::vector<std::string>{"ext:steam:r1"}));

    int source_checks = 0;
    auto local = base_deps(&embedded);
    local.existingCount = [](const GameRef&) { return 3; };
    local.resolveSteam = [&](const GameRef&) {
        ++source_checks;
        return std::optional<SteamApp>();
    };
    const auto di_r = collect_game_voices(game, local);
    CHECK(di_r.channel == "di" && di_r.collected == 3 && source_checks == 0 && !di_r.gameSlug);

    auto yt = base_deps(&embedded);
    yt.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(SteamApp{7, "新作", true}); };
    yt.fetchSteam = [](const SteamApp&, const std::string&) { return std::vector<cs::ExternalUtterance>{}; };
    yt.fetchYoutube = [](const GameRef&, const std::string& slug) {
        return std::vector<cs::ExternalUtterance>{utt("youtube", "y1", slug, "video", "操作が楽しい", 1, "anonymous")};
    };
    const auto ytr = collect_game_voices(game, yt);
    CHECK_EQ(ytr.channel, std::string("youtube"));
    CHECK_EQ(ytr.gameSlug.value_or(""), "youtube-" + glab_game_slug(game.title));

    auto un = base_deps(&embedded);
    un.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(SteamApp{7, "新作", false}); };
    const auto unr = collect_game_voices(game, un);
    CHECK(unr.channel == "glab" && unr.unreleased);
    CHECK_EQ(unr.gameSlug.value_or(""), glab_game_slug(game.title));
    CHECK((embedded.back() == std::vector<std::string>{"ext:glab:v1"}));

    auto nf = base_deps(&embedded);
    nf.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(); };
    nf.fetchGlab = nullptr;
    const auto nfr = collect_game_voices(game, nf);
    CHECK(nfr.channel == "none" && !nfr.gameSlug && !nfr.voluptasDown);

    std::vector<std::string> warnings;
    auto failing = base_deps(&embedded);
    failing.resolveSteam = [](const GameRef&) -> std::optional<SteamApp> { throw std::runtime_error("network"); };
    failing.fetchGlab = [](const GameRef&, const std::string&) -> std::vector<cs::ExternalUtterance> { throw std::runtime_error("401"); };
    failing.warn = [&](const std::string& m) { warnings.push_back(m); };
    const auto fr = collect_game_voices(game, failing);
    CHECK(fr.channel == "none" && warnings.size() == 2);
    CHECK(fr.voluptasDown.has_value() && !*fr.voluptasDown);
    // ベクトル化の失敗は取込を止めない
    auto embfail = base_deps(&embedded);
    embfail.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(SteamApp{7, "新作", true}); };
    embfail.embed = [](core::Core*, const std::vector<std::string>&) -> int { throw std::runtime_error("embed down"); };
    std::vector<std::string> ew;
    embfail.warn = [&](const std::string& m) { ew.push_back(m); };
    const auto efr = collect_game_voices(game, embfail);
    CHECK(efr.channel == "steam" && efr.imported == 1 && efr.embedded == 0 && ew.size() == 1);
    // 複数ゲームは順に
    const auto all = collect_user_voices({game, similar_game_ref("別")}, d);
    CHECK(all.size() == 2 && all[1].title == "別");
}

TEST_CASE("collect: 届いていた感想への fallback (Voluptas 停止 / 未設定 / 0 件)") {
    std::vector<std::vector<std::string>> embedded;
    const GameRef game{"テストゲーム", {"テストゲーム"}, "テストゲーム"};
    auto d = base_deps(&embedded);
    d.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(); };
    d.fetchGlab = [](const GameRef&, const std::string&) -> std::vector<cs::ExternalUtterance> { throw VoluptasUnavailable(); };
    d.storedGlabCount = [](const std::string& slug) { return slug == glab_game_slug("テストゲーム") ? 3 : 0; };
    const auto down = collect_game_voices(game, d);
    CHECK(down.channel == "glab" && down.collected == 3 && down.gameSlug == glab_game_slug("テストゲーム"));
    d.storedGlabCount = [](const std::string&) { return 0; };
    const auto down_none = collect_game_voices(game, d);
    CHECK(down_none.channel == "none" && down_none.voluptasDown.value_or(false));
    CHECK(str::contains(describe_voice_report(down_none), "Voluptas（Vo）が停止中"));

    auto nofetch = base_deps(&embedded);
    nofetch.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(); };
    nofetch.fetchGlab = nullptr;
    nofetch.storedGlabCount = [](const std::string&) { return 2; };
    CHECK_EQ(collect_game_voices(game, nofetch).channel, std::string("glab"));

    auto empty = base_deps(&embedded);
    empty.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(); };
    empty.fetchGlab = [](const GameRef&, const std::string&) { return std::vector<cs::ExternalUtterance>{}; };
    empty.storedGlabCount = [](const std::string&) { return 0; };
    CHECK_EQ(collect_game_voices(game, empty).channel, std::string("none"));
}

// ── importer (importer.test.ts / summary.test.ts) ────────────────────────

TEST_CASE("importer: Steam 風の取込・冪等・responds_to・2 層化・出所メタ") {
    test::TempDir dir;
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    auto ingested = cs::open_ingested_store(dir.file("ingested.sqlite"));
    cs::ExternalImportOptions opts;
    opts.workspaceId = "knowledge";
    opts.ingested = ingested.get();

    auto r1i = utt("steam", "r1", "vs", "1794680", "面白い", 1000, "S1", "https://example/r1");
    r1i.signal = cs::ExternalSignal{true, 5.0};
    auto r2i = utt("steam", "r2", "vs", "1794680", "重い", 2000, "S2", "https://example/r2");
    r2i.signal = cs::ExternalSignal{false, std::nullopt};
    const std::vector<cs::ExternalUtterance> steam{r1i, r2i};
    const auto r1 = cs::import_external_utterances(*core, steam, opts);
    CHECK(r1.sessions == 1 && r1.utterances == 2 && r1.reactions == 2 && r1.skipped == 0);
    const auto u1 = core->raw().get("SELECT * FROM utterances WHERE id = ?", {"ext:steam:r1"});
    CHECK(u1 && u1->str("speaker_id") == "ext:steam:S1" && u1->str("session_id") == "ext:steam:1794680" && u1->str("raw_content") == "面白い");
    const auto react = core->raw().get("SELECT * FROM reactions WHERE utterance_id = ?", {"ext:steam:r1"});
    CHECK(react && react->str("reaction_type") == "positive" && react->num("intensity") == 5);
    const auto session = core->raw().get("SELECT * FROM sessions WHERE id = ?", {"ext:steam:1794680"});
    CHECK(session && session->str("title") == "steam:vs");

    const auto r2 = cs::import_external_utterances(*core, steam, opts);
    CHECK(r2.utterances == 0 && r2.skipped == 2);
    CHECK_EQ(core->raw().get("SELECT COUNT(*) AS c FROM utterances")->i64("c"), 2);
    // sidecar 無しでも DB にあれば skip (sidecar に記録し直す)
    auto ingested2 = cs::open_ingested_store(dir.file("ingested2.sqlite"));
    opts.ingested = ingested2.get();
    const auto r2b = cs::import_external_utterances(*core, steam, opts);
    CHECK(r2b.utterances == 0 && r2b.skipped == 2 && ingested2->has("steam", "r1"));
    opts.ingested = ingested.get();

    const std::vector<cs::ExternalUtterance> threaded{
        [] {
            auto c = utt("reddit", "c2", "vs", "t1", "返信", 200, "U2");
            c.parentNativeId = "c1";
            return c;
        }(),
        utt("reddit", "c1", "vs", "t1", "親", 100, "U1")};
    const auto r3 = cs::import_external_utterances(*core, threaded, opts);
    CHECK_EQ(r3.utterances, 2.0);
    CHECK_EQ(core->raw().get("SELECT responds_to FROM utterances WHERE id = ?", {"ext:reddit:c2"})->str("responds_to"), std::string("ext:reddit:c1"));
    ingested->close();
    ingested2->close();

    // 2 層化 (summary → 本文、raw → raw-store)、出所メタ、タイトル生成
    auto raw = cs::open_raw_store(dir.file("raw.sqlite"));
    auto attribution = cs::open_attribution_store(dir.file("attr.sqlite"));
    const std::string long_text = str::repeat("ワンダと巨像のレビュー本文。", 60);
    auto lu = utt("website", "u-long", "sotc", "t1", long_text, 1, "famitsu.com", "https://famitsu.com/a");
    lu.summary = " 要約(長文) ";
    auto su = utt("website", "u-short", "sotc", "t2", "短い感想", 2, "blog.test", "https://blog.test/b");
    cs::ExternalImportOptions o2;
    o2.workspaceId = "knowledge";
    o2.rawStore = raw.get();
    o2.attribution = attribution.get();
    o2.sessionTitle = [](const cs::ExternalUtterance& i) { return "T:" + i.threadKey; };
    const auto rs = cs::import_external_utterances(*core, {lu, su}, o2);
    CHECK(rs.utterances == 2 && rs.summarized == 1);
    CHECK_EQ(core->raw().get("SELECT raw_content FROM utterances WHERE id = ?", {"ext:website:u-long"})->str("raw_content"), std::string("要約(長文)"));
    CHECK_EQ(core->raw().get("SELECT raw_content FROM utterances WHERE id = ?", {"ext:website:u-short"})->str("raw_content"), std::string("短い感想"));
    const auto doc = raw->get("ext:website:u-long");
    CHECK(doc && doc->rawText == long_text && doc->url == "https://famitsu.com/a");
    CHECK(!raw->get("ext:website:u-short"));
    const auto at = attribution->get("ext:website:u-long");
    CHECK(at && at->source == "website" && at->sourceUrl == "https://famitsu.com/a" && at->nativeId == "u-long" && at->gameSlug == std::string("sotc"));
    CHECK_EQ(core->raw().get("SELECT title FROM sessions WHERE id = ?", {"ext:website:t1"})->str("title"), std::string("T:t1"));
    // rawStore なしなら raw を本文に
    auto lu2 = lu;
    lu2.nativeId = "u-long2";
    cs::ExternalImportOptions o3;
    o3.workspaceId = "knowledge";
    const auto r4 = cs::import_external_utterances(*core, {lu2}, o3);
    CHECK_EQ(r4.summarized, 0.0);
    CHECK_EQ(core->raw().get("SELECT raw_content FROM utterances WHERE id = ?", {"ext:website:u-long2"})->str("raw_content"), long_text);
    raw->close();
    attribution->close();
    core->close();
}

TEST_CASE("importer: postedAt 昇順で入れる (session は最初の時刻)") {
    test::TempDir dir;
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    cs::ExternalImportOptions opts;
    opts.workspaceId = "knowledge";
    const auto r = cs::import_external_utterances(*core, {utt("reddit", "c", "g", "t", "c", 3, "C"), utt("reddit", "b", "g", "t", "b", 2, "B"), utt("reddit", "a", "g", "t", "a", 1, "A")}, opts);
    CHECK_EQ(r.utterances, 3.0);
    // session は最初に入った項目 (postedAt 最小) の時刻で作られる
    CHECK_EQ(core->raw().get("SELECT started_at FROM sessions WHERE id = ?", {"ext:reddit:t"})->num("started_at"), 1.0);
    core->close();
}

// ── listGameVoices (一時 KG) ────────────────────────────────────────────

TEST_CASE("list_game_voices: gameSlug で引き、議題ベクトルに近い順・未ベクトル化は後ろ (新しい順)") {
    test::TempDir dir;
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    auto attribution = cs::open_attribution_store(dir.file(".attribution.sqlite"));
    const std::string slug = steam_game_slug(7);
    std::vector<cs::ExternalUtterance> items;
    const std::vector<std::string> contents{"ガチャが渋い", "周回が楽しい", "グラフィックが綺麗"};
    for (std::size_t i = 0; i < contents.size(); ++i)
        items.push_back(utt("steam", "r" + std::to_string(i), slug, "7", contents[i], 1000.0 + static_cast<double>(i), "S" + std::to_string(i),
                            "https://example/r" + std::to_string(i)));
    items.push_back(utt("steam", "blank", slug, "7", "   ", 5000, "S9"));
    cs::ExternalImportOptions opts;
    opts.workspaceId = "knowledge";
    opts.attribution = attribution.get();
    cs::import_external_utterances(*core, items, opts);
    core->register_embedding({"knowledge", "utterance", "ext:steam:r0", {0, 1}});
    core->register_embedding({"knowledge", "utterance", "ext:steam:r1", {1, 0}});

    ListGameVoicesArgs a;
    a.core = core.get();
    a.attribution = attribution.get();
    a.workspaceId = "knowledge";
    a.gameSlug = slug;
    a.limit = 3;
    a.queryVector = std::vector<double>{1, 0.1};
    const auto ranked = list_game_voices(a);
    CHECK(ranked.size() == 3 && ranked[0].content == "周回が楽しい" && ranked[1].content == "ガチャが渋い" && ranked[2].content == "グラフィックが綺麗");
    CHECK(ranked[0].source == "steam" && ranked[0].sourceUrl == std::string("https://example/r1"));
    a.gameSlug = "steam-999";
    CHECK(list_game_voices(a).empty());
    a.gameSlug = slug;
    a.limit = 2;
    a.queryVector.reset();
    CHECK_EQ(list_game_voices(a).size(), std::size_t{2});
    a.limit = -1;
    CHECK(list_game_voices(a).empty());
    a.workspaceId = "other";
    a.limit = 10;
    CHECK(list_game_voices(a).empty());
    attribution->close();
    core->close();
}

// ── voice-index ──────────────────────────────────────────────────────────

TEST_CASE("voice_index: 未ベクトル化の声だけを埋める・2000 文字で切る・バッチ・ベクトル不足は失敗") {
    test::TempDir dir;
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    cs::ExternalImportOptions opts;
    opts.workspaceId = "knowledge";
    const std::string long_text = str::repeat("あ", 2500);
    cs::import_external_utterances(*core,
                                   {utt("steam", "a", "g", "t", long_text, 1, "A"), utt("steam", "b", "g", "t", "b", 2, "B"),
                                    utt("steam", "c", "g", "t", "c", 3, "C")},
                                   opts);
    core->register_embedding({"knowledge", "utterance", "ext:steam:c", {1, 1}});
    FakeEmbedder emb;
    core::vectors::EmbedVoiceOptions o;
    o.workspaceId = "knowledge";
    o.batchSize = 1;
    std::size_t progress = 0;
    o.onProgress = [&](std::size_t done, std::size_t) { progress = done; };
    const auto n = core::vectors::embed_voices_by_ids(*core, emb, {"ext:steam:a", "ext:steam:b", "ext:steam:a", "ext:steam:c", "missing"}, o);
    CHECK_EQ(n, std::size_t{2});
    CHECK_EQ(progress, std::size_t{2});
    CHECK_EQ(emb.batches.size(), std::size_t{2});
    CHECK_EQ(str::js_length(emb.batches[0][0]), core::vectors::EMBED_TEXT_CAP);
    CHECK_EQ(core->raw().get("SELECT COUNT(*) AS c FROM embeddings")->i64("c"), 3);
    // 既に埋まっていれば何もしない
    CHECK_EQ(core::vectors::embed_voices_by_ids(*core, emb, {"ext:steam:a"}, o), std::size_t{0});
    CHECK_EQ(core::vectors::embed_voices_by_ids(*core, emb, {}, o), std::size_t{0});
    // 空白だけの行は対象外
    CHECK_EQ(core::vectors::embed_voice_rows(*core, emb, {core::vectors::VoiceRow{"x", std::string("  ")}, core::vectors::VoiceRow{"y", std::nullopt}}, o),
             std::size_t{0});
    FakeEmbedder short_emb;
    short_emb.short_reply = true;
    o.batchSize = 0;  // 1 に丸める
    CHECK_THROWS(core::vectors::embed_voice_rows(*core, short_emb, {core::vectors::VoiceRow{"z", std::string("text")}}, o));
    core->close();
}

// ── prepare_user_voices ─────────────────────────────────────────────────

TEST_CASE("prepare_user_voices: 無効化・Core 未設定・全滅は lookup 無し / 集めた声をゲームごとに均等に混ぜる") {
    test::TempDir dir;
    config::LoadOptions lo;
    lo.env = std::map<std::string, std::string>{};
    lo.cwd = dir.file("");
    auto cfg = config::load_config(lo);
    cfg.embedding.enabled = false;
    cfg.flow.userVoices.enabled = true;

    const std::string kg = dir.file("db.kuzu");
    const std::string attr = dir.file(".attribution.sqlite");
    {
        auto core = core::create_core(kg, dir.file("events.jsonl"));
        auto attribution = cs::open_attribution_store(attr);
        cs::ExternalImportOptions opts;
        opts.workspaceId = "knowledge";
        opts.attribution = attribution.get();
        cs::import_external_utterances(*core,
                                       {utt("steam", "a1", "steam-7", "7", "A1", 1, "A"), utt("steam", "a2", "steam-7", "7", "A2", 2, "A"),
                                        utt("glab", "b1", glab_game_slug("別ゲーム"), "別ゲーム", "B1", 3, "anonymous")},
                                       opts);
        attribution->close();
        core->close();
    }
    PrepareUserVoicesArgs args;
    args.theme = "「新作」の操作感";
    args.similarGames = {"別ゲーム"};
    args.workspaceId = "knowledge";
    args.config = &cfg;
    args.openCore = [&] { return core::create_core(kg, dir.file("events.jsonl")); };
    args.openAttribution = [&] { return cs::open_attribution_store(attr); };
    args.baseLookup = [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{ContextVoice{"[synthetic] x", "llm", std::nullopt}}; };
    args.collectDeps = [](core::Core* c) {
        CollectUserVoicesDeps d;
        d.core = c;
        d.workspaceId = "knowledge";
        d.resolveSteam = [](const GameRef& g) {
            return g.title == "別ゲーム" ? std::optional<SteamApp>() : std::optional<SteamApp>(SteamApp{7, "新作", true});
        };
        d.fetchSteam = [](const SteamApp&, const std::string& slug) {
            return std::vector<cs::ExternalUtterance>{utt("steam", "a1", slug, "7", "A1", 1, "A")};
        };
        d.storedGlabCount = [](const std::string&) { return 1; };
        d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>&, const std::string&) { return 0; };
        return d;
    };
    const auto prepared = prepare_user_voices(args);
    CHECK(prepared.reports.size() == 2 && prepared.reports[0].channel == "steam" && prepared.reports[1].channel == "glab");
    CHECK(static_cast<bool>(prepared.lookup));
    const auto mixed = prepared.lookup({"新作"}, 3);
    // 取込時刻が同じなので steam-7 の 2 件の順は問わない。ゲームごとに交互 (steam → glab → steam)。
    CHECK(mixed.size() == 3 && mixed[1].content == "B1");
    CHECK(((mixed[0].content == "A1" && mixed[2].content == "A2") || (mixed[0].content == "A2" && mixed[2].content == "A1")));

    auto disabled = cfg;
    disabled.flow.userVoices.enabled = false;
    auto a2 = args;
    a2.config = &disabled;
    CHECK(prepare_user_voices(a2).reports.empty());
    auto a3 = args;
    a3.openCore = nullptr;
    CHECK(!prepare_user_voices(a3).lookup);
    auto a4 = args;
    a4.collectDeps = [](core::Core*) { return CollectUserVoicesDeps{}; };
    const auto none = prepare_user_voices(a4);
    CHECK(none.reports.size() == 2 && !none.lookup);
    auto a5 = args;
    a5.includeThemeGame = false;
    a5.similarGames = {};
    CHECK(prepare_user_voices(a5).reports.empty());
}

DI_TEST_MAIN()
