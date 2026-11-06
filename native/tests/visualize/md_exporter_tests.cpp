// w3-N: md-exporter / dump。tests/visualize/md-exporter.test.ts の移植 + golden (gray-matter と 1 バイト一致) + 境界。
#include "check.hpp"
#include "w3n_support.hpp"

#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "lib/front_matter/front_matter.hpp"
#include "visualize/dump.hpp"
#include "visualize/md_exporter.hpp"

using namespace di;
using namespace di::visualize;

// Windows ではパスの区切りが \ になる (Node の path.join と同じ)。区切りを / にそろえて比べる。
static std::string slashed(const std::string& path) { return str::replace_all(path, "\\", "/"); }

TEST_CASE("golden: export_node の markdown と outgoing が Node (gray-matter) と 1 バイト一致") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    const Json fx = w3n::load_fixture("w3n/md_exporter.json");
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    w3n::load_tables(*core, fx["tables"]);
    for (const auto& c : fx["cases"]) {
        const std::string type = c["type"], id = c["id"];
        const auto& expected = c["result"];
        try {
            const auto r = export_node(*core, type, fx["workspaceId"], id);
            CHECK(expected["ok"].get<bool>());
            CHECK_EQ(r.markdown, expected["value"]["markdown"].get<std::string>());
            CHECK_EQ(json_stringify(to_json(r)), json_stringify(expected["value"]));
        } catch (const ExporterError& e) {
            CHECK(!expected["ok"].get<bool>());
            CHECK_EQ(std::string(e.what()), expected["error"].get<std::string>());
        }
    }
    core->close();
}

TEST_CASE("md-exporter.test.ts: front matter と本文 (再 parse して確かめる)・不存在 id は ExporterError") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    const Json fx = w3n::load_fixture("w3n/md_exporter.json");
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    w3n::load_tables(*core, fx["tables"]);
    const std::string ws = fx["workspaceId"];
    auto parsed = [](const ExportResult& r) { return lib::front_matter::parse(r.markdown)->data; };

    const auto h = export_hypothesis(*core, ws, "h1");
    const auto hd = parsed(h);
    CHECK_EQ(hd["type"].get<std::string>(), std::string("hypothesis"));
    CHECK_EQ(hd["status"].get<std::string>(), std::string("validated"));
    CHECK_EQ(hd["addresses"].get<std::string>(), std::string("[[gap:gap1]]"));
    CHECK_EQ(hd["refs"][0].get<std::string>(), std::string("[[utt:u1]]"));
    CHECK_EQ(hd["validated_by"][0].get<std::string>(), std::string("emotion"));
    bool has_mch = false;
    for (const auto& l : h.outgoing) has_mch |= l.type == "mch";
    CHECK(has_mch);  // intends LIKE 逆引きで Cast Spell を拾う

    const auto g = parsed(export_gap(*core, ws, "gap1"));
    CHECK_EQ(g["hypotheses"].size(), std::size_t{2});
    CHECK_EQ(parsed(export_mechanic(*core, ws, "m1"))["game"].get<std::string>(), std::string("[[game:g1]]"));
    const auto u = export_utterance(*core, ws, "u1");
    CHECK_EQ(parsed(u)["session"].get<std::string>(), std::string("[[ses:s1]]"));
    CHECK(str::contains(u.markdown, "> MP の意味がわからない"));
    CHECK_THROWS(export_hypothesis(*core, ws, "missing-id"));
    CHECK_THROWS(export_node(*core, "aff", ws, "x"));
    CHECK_THROWS(export_node(*core, "unknown", ws, "x"));
    core->close();
}

TEST_CASE("dump_node: created → unchanged → updated・root の外を指す id / workspace は拒否") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    const Json fx = w3n::load_fixture("w3n/md_exporter.json");
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    w3n::load_tables(*core, fx["tables"]);
    const std::string root = dir.file("out");
    DumpOptions opt{root, "knowledge"};
    auto r = dump_node(*core, "hyp", "h1", opt);
    CHECK_EQ(r.written, std::string("created"));
    CHECK_EQ(slashed(r.filePath), slashed(root) + "/knowledge/hypothesis/h1.md");
    CHECK_EQ(*fs::read_file(r.filePath), r.markdown);
    CHECK_EQ(dump_node(*core, "hyp", "h1", opt).written, std::string("unchanged"));
    core->raw().run("UPDATE hypotheses SET statement = 'changed' WHERE id = 'h1'");
    CHECK_EQ(dump_node(*core, "hyp", "h1", opt).written, std::string("updated"));
    CHECK_THROWS(resolve_file_path("hyp", "../../../escape", opt));
    CHECK_THROWS(resolve_file_path("hyp", "x", DumpOptions{root, "../.."}));
    CHECK_THROWS(resolve_file_path("hyp", "/abs", DumpOptions{root, ""}));
    CHECK_THROWS(resolve_file_path("nope", "x", opt));
    // root の中に収まる ".." は path.join と同じく正規化して許す。
    CHECK_EQ(slashed(resolve_file_path("hyp", "../../inside", opt)), slashed(root) + "/inside.md");
    CHECK_EQ(slashed(resolve_file_path("ses", "s1", opt)), slashed(root) + "/knowledge/session/s1.md");
    core->close();
}

TEST_CASE("境界: 壊れた / 巨大な refs_json は [] 扱い・範囲外の時刻は例外") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    const Json fx = w3n::load_fixture("w3n/md_exporter.json");
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    w3n::load_tables(*core, fx["tables"]);
    // 上限 (4MiB) + 1 の refs_json は読まない (refs 無し)。
    std::string big = "[\"" + std::string(4u << 20, 'a') + "\"]";
    core->raw().run("UPDATE hypotheses SET refs_json = ? WHERE id = 'h3'", {big});
    auto r = export_hypothesis(*core, "knowledge", "h3");
    CHECK(!str::contains(r.markdown, "## 根拠"));
    core->raw().run("UPDATE hypotheses SET refs_json = '[[[[' WHERE id = 'h3'");
    CHECK(!str::contains(export_hypothesis(*core, "knowledge", "h3").markdown, "## 根拠"));
    core->raw().run("UPDATE hypotheses SET created_at = 9e18 WHERE id = 'h3'");
    CHECK_THROWS(export_hypothesis(*core, "knowledge", "h3"));
    core->close();
}

DI_TEST_MAIN()
