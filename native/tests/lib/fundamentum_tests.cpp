// lib/fundamentum の移植テスト。
// TS: lib/fundamentum/src/__tests__/{hash,master,file-backend,user,resolver,integration}.test.ts
// golden: native/tests/golden/w1-A-fundamentum.gen.ts
#include <cmath>
#include <filesystem>
#include <limits>
#include <iostream>

#include "../check.hpp"
#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "lib/fundamentum/fundamentum.hpp"

using namespace di;
using namespace di::lib::fundamentum;

namespace {

std::string make_temp_dir() {
    auto dir = std::filesystem::temp_directory_path() / ("di-fm-test-" + crypto::uuid_v4());
    std::filesystem::create_directories(dir);
    return fs::from_path(std::filesystem::canonical(dir));
}

Json load_fixture(const std::string& name) {
    auto text = fs::read_file(test::fixtures_dir() + "/fundamentum/" + name);
    if (!text) throw std::runtime_error("fixture missing: " + name);
    return parse_json_or_throw(*text, JsonLimits{8u << 20, 1'000'000, 64});
}

void walk(const std::filesystem::path& d, const std::string& base, Json& out) {
    for (const auto& e : std::filesystem::directory_iterator(d)) {
        const std::string name = fs::from_path(e.path().filename());
        const std::string rel = base.empty() ? name : base + "/" + name;
        if (e.is_directory()) walk(e.path(), rel, out);
        else out[rel] = *fs::read_file(fs::from_path(e.path()));
    }
}

}  // namespace

TEST_CASE("golden: canonicalize / contentIdOf が Node と一致") {
    const Json cases = load_fixture("canonical.json");
    CHECK(cases.size() >= 20);
    for (const auto& c : cases) {
        CHECK_EQ(canonicalize(c["input"]), c["canonical"].get<std::string>());
        CHECK_EQ(content_id_of(c["input"]), c["id"].get<std::string>());
    }
}

TEST_CASE("golden: FileBackend の操作列が同じファイル配置と戻り値になる") {
    const Json fx = load_fixture("scenario.json");
    const std::string dir = make_temp_dir();
    auto fm = Foundation::on_disk(dir);
    Json results = Json::object();
    const auto r1 = fm->master.put(parse_json_or_throw(R"({"title":"議論A","utterances":[{"text":"x"}]})"));
    const auto r2 = fm->master.put(parse_json_or_throw(R"({"title":"議論B"})"));
    const auto r3 = fm->master.put(parse_json_or_throw(R"({"title":"議論A","utterances":[{"text":"x"}]})"));
    results["puts"] = Json::array({r1.id, r2.id, r3.id});
    const auto v1 = fm->catalog.builder("discutere/discussions").set("flow:s1", r1.id).set("gap-2", r2.id).commit();
    const auto v1b = fm->catalog.builder("discutere/discussions").set("flow:s1", r1.id).commit();
    const auto r4 = fm->master.put(parse_json_or_throw(R"({"title":"議論A2"})"));
    const auto v2 = fm->catalog.builder("discutere/discussions").set("flow:s1", r4.id).remove("gap-2").set("新規", r2.id).commit();
    results["catalogs"] = Json::array({to_json(v1), to_json(v1b), to_json(v2)});
    results["diff"] = to_json(fm->catalog.diff(v1, v2));
    Json hist = Json::array();
    for (const auto& c : fm->catalog.history("discutere/discussions")) hist.push_back(c.id);
    results["history"] = hist;
    results["version1"] = fm->catalog.version("discutere/discussions", 1)->id;
    results["names"] = fm->catalog.names();
    results["userSet1"] = to_json(fm->user.set("proj-1", "flow:s1", parse_json_or_throw(R"({"note":"上書き"})")));
    results["userSet2"] = to_json(fm->user.set("proj-1", "flow:s1", parse_json_or_throw(R"({"note":"二回目"})")));
    results["userDel"] = to_json(fm->user.remove("proj-1", "gap-x"));
    results["userList"] = fm->user.list("proj-1");
    Json all = Json::array();
    for (const auto& r : fm->resolve_all("proj-1", "discutere/discussions")) all.push_back(to_json(r));
    results["resolveAll"] = all;
    results["resolveNone"] = to_json(fm->resolve("proj-1", "discutere/discussions", "missing"));
    CHECK_EQ(json_stringify(results), json_stringify(fx["results"]));

    Json files = Json::object();
    walk(fs::to_path(dir), "", files);
    // ファイル一覧はキー順に依存しないよう canonical で比べる
    CHECK_EQ(canonicalize(files), canonicalize(fx["files"]));
    std::filesystem::remove_all(fs::to_path(dir));
}

TEST_CASE("hash: キー順に依存しない・非有限数で throw・isContentId") {
    CHECK_EQ(canonicalize(parse_json_or_throw(R"({"b":1,"a":2})")), canonicalize(parse_json_or_throw(R"({"a":2,"b":1})")));
    CHECK(canonicalize(parse_json_or_throw("[1,2,3]")) != canonicalize(parse_json_or_throw("[3,2,1]")));
    CHECK_EQ(canonicalize(Json(-0.0)), std::string("0"));
    CHECK_THROWS(canonicalize(Json(std::numeric_limits<double>::infinity())));
    CHECK_THROWS(canonicalize(Json(std::nan(""))));
    const auto id = content_id_of(parse_json_or_throw(R"({"hello":"world"})"));
    CHECK(is_content_id(id));
    CHECK(!is_content_id("not-an-id"));
    CHECK(!is_content_id("fm1:zzz"));
}

TEST_CASE("master / catalog (master.test)") {
    auto fm = Foundation::in_memory();
    const auto a = fm->master.put(parse_json_or_throw(R"({"x":1,"y":2})"));
    const auto b = fm->master.put(parse_json_or_throw(R"({"y":2,"x":1})"));
    CHECK_EQ(a.id, b.id);
    CHECK(!fm->master.get("fm1:" + std::string(64, '0')).has_value());
    const auto ra = fm->master.put(parse_json_or_throw(R"({"v":"a"})"));
    const auto ra2 = fm->master.put(parse_json_or_throw(R"({"v":"a2"})"));
    const auto rb = fm->master.put(parse_json_or_throw(R"({"v":"b"})"));
    const auto rc = fm->master.put(parse_json_or_throw(R"({"v":"c"})"));
    const auto v1 = fm->catalog.builder("an/proj").set("keep", rb.id).set("change", ra.id).set("drop", rc.id).commit();
    CHECK_EQ(v1.version, std::int64_t{1});
    CHECK(!v1.parent.has_value());
    const auto v2 = fm->catalog.builder("an/proj").set("change", ra2.id).remove("drop").set("new", rc.id).commit();
    CHECK_EQ(v2.version, std::int64_t{2});
    CHECK_EQ(*v2.parent, v1.id);
    const auto d = fm->catalog.diff(v1, v2);
    CHECK_EQ(d.added, std::vector<std::string>{"new"});
    CHECK_EQ(d.removed, std::vector<std::string>{"drop"});
    CHECK_EQ(d.changed, std::vector<std::string>{"change"});
    CHECK_EQ(d.unchanged, std::vector<std::string>{"keep"});
    const auto noop = fm->catalog.builder("an/proj").set("change", ra2.id).commit();
    CHECK_EQ(noop.id, v2.id);
    CHECK_EQ(fm->catalog.history("an/proj").size(), std::size_t{2});
    fm->catalog.builder("an/b").set("x", ra.id).commit();
    CHECK_EQ(fm->catalog.names(), (std::vector<std::string>{"an/b", "an/proj"}));
}

TEST_CASE("user / resolver (user.test / resolver.test)") {
    auto fm = Foundation::in_memory();
    const auto rule = fm->master.put(parse_json_or_throw(R"({"rule":"no-cross-domain","severity":"block"})"));
    fm->catalog.builder("an/proj").set("rule:ncd", rule.id).commit();
    auto r = fm->resolve("p1", "an/proj", "rule:ncd");
    CHECK_EQ(r.source, std::string("master"));
    CHECK_EQ(*r.id, rule.id);
    fm->user.set("p1", "rule:ncd", parse_json_or_throw(R"({"severity":"warn"})"));
    r = fm->resolve("p1", "an/proj", "rule:ncd");
    CHECK_EQ(r.source, std::string("user"));
    CHECK(!r.id.has_value());
    CHECK_EQ(fm->resolve("p2", "an/proj", "rule:ncd").source, std::string("master"));
    const auto t = fm->user.remove("p1", "rule:ncd");
    CHECK(t.deleted);
    CHECK_EQ(t.revision, std::int64_t{2});
    r = fm->resolve("p1", "an/proj", "rule:ncd");
    CHECK_EQ(r.source, std::string("user"));
    CHECK(r.value.is_null());
    CHECK(!fm->user.get("p1", "rule:ncd").has_value());
    CHECK_EQ(fm->resolve("p1", "an/proj", "none").source, std::string("none"));
    fm->user.set("p", "a", Json(1));
    fm->user.set("p", "b", Json(2));
    fm->user.remove("p", "b");
    CHECK_EQ(fm->user.list("p"), std::vector<std::string>{"a"});
    CHECK_EQ(fm->user.history("p", "b").size(), std::size_t{2});
}

TEST_CASE("file backend: fs 非安全な key・永続・不正 id・壊れた中身") {
    const std::string dir = make_temp_dir();
    {
        FileBackend be(dir);
        be.put_keyed("an/proj@v", "rule:a/b 日本語", "ok");
        CHECK_EQ(*be.get_keyed("an/proj@v", "rule:a/b 日本語"), std::string("ok"));
        CHECK_EQ(be.list_keys("an/proj@v"), std::vector<std::string>{"rule:a/b 日本語"});
        CHECK(be.list_keys("nope").empty());
        be.put_keyed("part", "k1", "v1");
        be.put_keyed("part", "k2", "v2");
        be.delete_keyed("part", "k1");
        be.delete_keyed("part", "k1");  // 不在でも no-op
        CHECK_EQ(be.list_keys("part"), std::vector<std::string>{"k2"});
        be.put_content("fm1:" + std::string(64, 'a'), R"({"x":1})");
        CHECK(be.has_content("fm1:" + std::string(64, 'a')));
        CHECK(!be.get_content("fm1:" + std::string(64, 'f')).has_value());
        // パスを脱出しうる id は読まない・書かない
        CHECK(!be.get_content("fm1:../../etc/passwd").has_value());
        CHECK(!be.has_content("../x"));
        CHECK_THROWS(be.put_content("fm1:../evil", "{}"));
    }
    {
        auto fm1 = Foundation::on_disk(dir);
        const auto r = fm1->master.put(parse_json_or_throw(R"({"rule":"a"})"));
        fm1->catalog.builder("an/proj").set("rule:a", r.id).commit();
        fm1->user.set("p1", "rule:a", parse_json_or_throw(R"({"local":true})"));
        auto fm2 = Foundation::on_disk(dir);
        CHECK_EQ(json_stringify(*fm2->master.get(r.id)), std::string(R"({"rule":"a"})"));
        CHECK_EQ(fm2->catalog.latest("an/proj")->entries["rule:a"].get<std::string>(), r.id);
        CHECK_EQ(json_stringify(*fm2->user.get("p1", "rule:a")), std::string(R"({"local":true})"));
        // 壊れた中身は例外 (TS の JSON.parse と同じ)。
        fm2->backend->put_content(content_id_of(Json("broken")), "{not json");
        CHECK_THROWS(fm2->master.get(content_id_of(Json("broken"))));
    }
    std::filesystem::remove_all(fs::to_path(dir));
}

TEST_CASE("境界: 保存値の読み戻し上限 (ちょうど・+1・深すぎる入れ子)") {
    auto fm = Foundation::in_memory();
    const std::string id = "fm1:" + std::string(64, 'b');
    std::string ok = "\"" + std::string(kMaxStoredBytes - 2, 'x') + "\"";
    fm->backend->put_content(id, ok);
    CHECK(fm->master.get(id).has_value());
    fm->backend->put_content(id, ok + " ");
    CHECK_THROWS(fm->master.get(id));
    fm->backend->put_content(id, std::string(500, '[') + std::string(500, ']'));
    CHECK_THROWS(fm->master.get(id));
}

DI_TEST_MAIN()
