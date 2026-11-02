// @ludiars/blackbox store-jsonfile.test.ts の移植 (一時ディレクトリ)。
#include "check.hpp"

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "lib/blackbox/store_jsonfile.hpp"

#include <filesystem>

using namespace di;
using namespace di::lib::blackbox;

namespace {
const Json COND = Json{{"op", "cmp"}, {"feature", "x"}, {"cmp", ">="}, {"value", 1}};
std::string tmp_path() {
    const auto dir = std::filesystem::temp_directory_path() / ("bb-file-" + crypto::uuid_v4());
    return fs::from_path(dir / "blackbox.json");
}
DecisionInput sample_input() {
    DecisionInput in;
    in.domain = "d";
    in.features = Json{{"x", 2}};
    in.output = Json{{"y", 1}};
    in.source = "rule";
    in.confidence = 0.7;
    in.rationale = "t";
    in.status = "pending_review";
    in.createdAt = "2026-07-02T00:00:00.000Z";
    return in;
}
} // namespace

TEST_CASE("rules / ledger round-trip and persist across instances") {
    const auto path = tmp_path();
    auto file = std::make_shared<BlackboxJsonFile>(path);
    JsonFileRuleStore store(file);
    RuleDraft d;
    d.domain = "d";
    d.description = "r";
    d.when = COND;
    d.output = Json{{"y", 1}};
    auto rule = store.insert(d);
    CHECK_EQ(store.find_by_fingerprint("d", rule.fingerprint)->id, rule.id);
    RulePatch p;
    p.state = "trial";
    store.update(rule.id, p);

    JsonFileDecisionLedger ledger(file);
    auto in = sample_input();
    in.ruleId = rule.id;
    auto id = ledger.record(in);
    CHECK_EQ(ledger.list_pending(std::string("d")).size(), std::size_t(1));

    JsonFileRuleStore reopened(std::make_shared<BlackboxJsonFile>(path));
    CHECK_EQ(reopened.get(rule.id)->state, std::string("trial"));
    JsonFileDecisionLedger reopened_ledger(std::make_shared<BlackboxJsonFile>(path));
    CHECK_EQ(json_stringify(reopened_ledger.get(id)->features), std::string(R"({"x":2})"));
    // ファイルの形 (JSON.stringify(state, null, 2) + "\n")。
    auto text = fs::read_file(path);
    CHECK(text->back() == '\n');
    CHECK(text->find("\n  \"rules\": [\n") != std::string::npos);
    CHECK(text->find("\"seq\": 1\n}") != std::string::npos);
}

TEST_CASE("makeFileBlackBox learning loop (propose -> shadow promote -> trial firing)") {
    auto bb = make_file_blackbox(tmp_path());
    int calls = 0;
    for (int i = 0; i < 4; ++i) {
        bb.engine->decide("d", Json::object(), Json{{"x", 5}}, [&](const Json&, const Json&, const LlmContext&) {
            calls += 1;
            LlmJudgement j;
            j.output = Json{{"y", 1}};
            j.confidence = 0.9;
            j.rationale = "t";
            j.proposedRule = RuleProposal{"x>=1 は y=1", COND, Json{{"y", 1}}, std::nullopt, std::nullopt};
            return j;
        });
    }
    CHECK_EQ(bb.rules->list_by_domain("d")[0].state, std::string("trial"));
    CHECK_EQ(calls, 4);
    auto r = bb.engine->decide("d", Json::object(), Json{{"x", 5}}, [](const Json&, const Json&, const LlmContext&) -> LlmJudgement {
        throw std::runtime_error("LLM は呼ばれないはず");
    });
    CHECK_EQ(r.decision.source, std::string("rule"));
    CHECK_EQ(bb.stats("d").ruleStates.trial, std::int64_t(1));
}

TEST_CASE("ledger is trimmed to maxDecisions") {
    JsonFileStoreOptions o;
    o.maxDecisions = 3;
    auto bb = make_file_blackbox(tmp_path(), {}, o);
    for (int i = 0; i < 5; ++i) {
        auto in = sample_input();
        in.source = "llm";
        in.status = "auto";
        bb.ledger->record(in);
    }
    auto recent = bb.ledger->list_recent("d", 100);
    CHECK_EQ(recent.size(), std::size_t(3));
    CHECK_EQ(recent[0].id, std::int64_t(5));
}

TEST_CASE("broken JSON is treated as empty state (does not throw)") {
    const auto path = tmp_path();
    BlackboxJsonFile file(path);
    file.save({});
    fs::write_file_atomic(path, "{broken");
    JsonFileRuleStore store(std::make_shared<BlackboxJsonFile>(path));
    CHECK(store.list_by_domain("d").empty());
    // 形の違う要素は読み飛ばす。
    fs::write_file_atomic(path, R"({"rules":[1,{"no":"id"},{"id":"r","domain":"d"}],"decisions":"x","seq":"y"})");
    CHECK_EQ(store.list_by_domain("d").size(), std::size_t(1));
}

TEST_CASE("oversized state file is refused instead of being silently overwritten") {
    const auto path = tmp_path();
    BlackboxJsonFile file(path);
    file.save({});
    std::string big(kBlackboxFileMaxBytes + 1, ' ');
    fs::write_file_atomic(path, big);
    JsonFileRuleStore store(std::make_shared<BlackboxJsonFile>(path));
    CHECK_THROWS(store.list_by_domain("d"));
    RuleDraft d;
    d.domain = "d";
    d.when = COND;
    CHECK_THROWS(store.insert(d));
    CHECK_EQ(*fs::file_size(path), static_cast<std::uintmax_t>(kBlackboxFileMaxBytes + 1));
}

DI_TEST_MAIN()
