// handler.ts の移植の検証: Node の golden (handleAction / extractJson / checkForbiddenRule) と
// tests/persona-engine/rule-self-edit.test.ts の観点、LLM 応答 (外部入力) の境界。
#include "check.hpp"

#include <nlohmann/json.hpp>

#include "base/strings.hpp"
#include "persona_engine/db/migrations.hpp"
#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/db/rules_repo.hpp"
#include "persona_engine/engine/handler.hpp"
#include "persona_engine/seeds/personas.hpp"
#include "persona_engine/seeds/rules.hpp"
#include "w2i_support.hpp"

using namespace di;
using namespace di::persona_engine;
using namespace di::persona_engine::engine;

namespace {
nlohmann::json plain(const Json& j) { return nlohmann::json::parse(j.dump()); }

void check_json(const Json& actual, const Json& expected, const std::string& what) {
    if (plain(actual) != plain(expected))
        throw test::Failure(what + "\n  actual:   " + actual.dump() + "\n  expected: " + expected.dump());
}

Json opt_json(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }
} // namespace

TEST_CASE("extract_json: Node golden") {
    for (const auto& c : w2i::load_fixture("extract_json.json")) {
        const auto out = extract_json(c["input"].get<std::string>());
        if (c["output"].is_null()) {
            CHECK(!out);
        } else {
            CHECK(out.has_value());
            check_json(*out, c["output"], "extract: " + c["input"].get<std::string>());
        }
    }
}

TEST_CASE("extract_json: 巨大入力・試行回数の上限") {
    // 1 MiB を超える JSON は読まない (外部入力の上限)
    std::string big = R"({"action":"skip","pad":")" + std::string(1u << 20, 'x') + R"("})";
    CHECK(!extract_json(big));
    std::string ok = R"({"action":"skip","pad":")" + std::string((1u << 20) - 64, 'x') + R"("})";
    CHECK(extract_json(ok).has_value());
    // 病的な "{"k":"}{}{}..." でも打ち切る (O(n^2) にしない)
    std::string patho = "{\"k\":\"}";
    for (int i = 0; i < 100000; ++i) patho += "{}";
    CHECK(!extract_json(patho));
    // 文字列内の波括弧で候補を増やす: {"a":"}{}{}...{"} は最後の候補で初めて正しい JSON になる。
    auto build = [](int pairs) {
        std::string s = "note: {\"a\":\"}";
        for (int i = 0; i < pairs; ++i) s += "{}";
        return s + "{\"}";
    };
    CHECK(extract_json(build(kMaxExtractAttempts - 2)).has_value());  // 試行 = 上限ちょうど
    CHECK(!extract_json(build(kMaxExtractAttempts - 1)));              // 試行 = 上限+1 → 打ち切り
    // 壊れた入力
    CHECK(!extract_json(std::string("\xff\xfe{\"a\":\xff}")));
}

TEST_CASE("check_forbidden_rule: Node golden") {
    for (const auto& c : w2i::load_fixture("forbidden.json")) {
        const auto& in = c["input"];
        const auto r = check_forbidden_rule(in["id"].get<std::string>(),
                                            in["description"].is_null() ? std::nullopt : std::optional<std::string>(in["description"].get<std::string>()),
                                            in["instructions"].get<std::string>());
        CHECK_EQ(r.ok, c["output"]["ok"].get<bool>());
        if (!r.ok) CHECK_EQ(r.reason, c["output"]["reason"].get<std::string>());
    }
    // 巨大な説明文でも線形で終わる
    std::string huge(2'000'000, ' ');
    CHECK(check_forbidden_rule("x", "全" + huge + "x", "y").ok);
}

TEST_CASE("handle_action: Node golden (結果・rule_log・logger・provider 呼び出し・rules 表)") {
    for (const auto& c : w2i::load_fixture("handle_action.json")) {
        const auto& hc = c["case"];
        const std::string name = hc["name"].get<std::string>();
        auto db = w2i::temp_db("handler");
        db::apply_persona_engine_migrations(*db);
        db::PersonasRepo(db).bulk_seed(seeds::DISCUSSION_PERSONA_SEEDS());
        db::RulesRepo rules(db);
        rules.bulk_seed(seeds::DISCUSSION_RULE_SEEDS());
        if (hc.contains("preAddRule")) {
            RuleSeed s;
            s.id = hc["preAddRule"].get<std::string>();
            s.trigger_type = "tick";
            s.tick_sec = 60;
            s.instructions = "これは十分に長い指示文で、議論の論点を一つ選び具体的な提案をする。";
            rules.insert_or_ignore(s, std::string("ai:x"));
        }
        const auto first = db->get("SELECT COALESCE(MAX(id),0) AS m FROM rule_log")->i64("m");
        w2i::RecordingProvider ctx;
        if (hc.contains("findSession")) {
            ctx.hasFind = true;
            if (!hc["findSession"].is_null()) ctx.findResult = hc["findSession"].get<std::string>();
        }
        if (hc.contains("findActive")) {
            ctx.hasActive = true;
            if (!hc["findActive"].is_null()) ctx.activeResult = hc["findActive"].get<std::string>();
        }
        w2i::RecordingLogger logger;
        HandleActionArgs args;
        args.ruleId = hc.value("ruleId", std::string("meta-improve-rules"));
        args.personaId = hc.value("personaId", std::string("neutral-observer"));
        args.workspaceId = "knowledge";
        if (hc.contains("sessionId") && !hc["sessionId"].is_null()) args.sessionId = hc["sessionId"].get<std::string>();
        if (hc.contains("triggerEvent")) {
            TriggerEvent t;
            t.kind = hc["triggerEvent"]["kind"].get<std::string>();
            if (hc["triggerEvent"].contains("payload")) t.payload = hc["triggerEvent"]["payload"];
            args.triggerEvent = t;
        }
        args.rawText = hc["rawText"].get<std::string>();
        args.contextProvider = &ctx;
        args.rules = &rules;
        args.logger = &logger;
        const auto r = handle_action(args);

        Json result = Json::object();
        result["kind"] = r.kind;
        result["detail"] = r.detail;
        if (r.createdId) result["createdId"] = *r.createdId;
        check_json(result, c["result"], name + ": result");

        Json logs = Json::array();
        for (const auto& row : db->all("SELECT rule_id, action, actor, detail FROM rule_log WHERE id > ? ORDER BY id", {first})) {
            Json l = Json::object();
            l["rule_id"] = row["rule_id"].is_null() ? Json(nullptr) : Json(row.str("rule_id"));
            l["action"] = row.str("action");
            l["actor"] = row.str("actor");
            l["detail"] = row["detail"].is_null() ? Json(nullptr) : Json(row.str("detail"));
            logs.push_back(l);
        }
        check_json(logs, c["logs"], name + ": logs");
        Json lc = Json::array();
        for (const auto& x : logger.calls) lc.push_back(x);
        check_json(lc, c["loggerCalls"], name + ": logger");

        Json proposed = Json::array();
        for (const auto& p : ctx.proposed) {
            Json j = Json::object();
            j["workspaceId"] = p.workspaceId;
            j["statement"] = p.statement;
            j["designGapId"] = opt_json(p.designGapId);
            j["byPersonaId"] = p.byPersonaId;
            if (p.reasoning) j["reasoning"] = *p.reasoning;
            proposed.push_back(j);
        }
        // TS は reasoning: undefined を JSON に出さない
        check_json(proposed, c["proposed"], name + ": proposed");
        Json posted = Json::array();
        for (const auto& p : ctx.posted) {
            Json j = Json::object();
            j["workspaceId"] = p.workspaceId;
            j["sessionId"] = p.sessionId;
            j["text"] = p.text;
            j["byPersonaId"] = p.byPersonaId;
            if (p.respondsTo) j["respondsTo"] = *p.respondsTo;
            posted.push_back(j);
        }
        check_json(posted, c["posted"], name + ": posted");
        Json finds = Json::array();
        for (const auto& [fn, q] : ctx.findCalls) finds.push_back(Json{{"fn", fn}, {"q", q}});
        check_json(finds, c["findCalls"], name + ": findCalls");

        std::vector<sql::Value> seed_ids;
        std::string marks;
        for (const auto& s : seeds::DISCUSSION_RULE_SEEDS()) {
            seed_ids.emplace_back(s.id);
            marks += marks.empty() ? "?" : ",?";
        }
        Json after = Json::array();
        for (const auto& row : db->all("SELECT id, description, trigger_type, tick_sec, event_kind, target, cooldown_sec, instructions, "
                                       "added_by, enabled, removed_by, removed_reason FROM rules WHERE id NOT IN (" + marks + ") ORDER BY id",
                                       sql::Params(seed_ids))) {
            Json j = Json::object();
            for (std::size_t i = 0; i < row.size(); ++i) {
                const auto& v = row.value(i);
                if (v.is_null()) j[row.name(i)] = nullptr;
                else if (v.is_int()) j[row.name(i)] = v.as_int();
                else j[row.name(i)] = v.as_string();
            }
            after.push_back(j);
        }
        check_json(after, c["rulesAfter"], name + ": rulesAfter");
    }
}

TEST_CASE("rule-self-edit: SEED_RULE_IDS が保護される") {
    CHECK(seeds::SEED_RULE_IDS().count("propose-on-gap"));
    CHECK(seeds::SEED_RULE_IDS().count("meta-improve-rules"));
    CHECK(!seeds::SEED_RULE_IDS().count("extra-counter"));
}

DI_TEST_MAIN()
