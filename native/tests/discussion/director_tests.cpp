// tests/discussion/director.test.ts の移植 (偽の LLM / 依存注入) + golden で Node と同じ進行・プロンプトを確かめる。
#include "check.hpp"

#include <set>

#include "base/fsutil.hpp"
#include "discussion/director.hpp"

using namespace di;
using namespace di::discussion;

namespace {
Rng seq_rng(std::vector<double> values) {
    auto i = std::make_shared<std::size_t>(0);
    return [values, i] { return values[(*i)++ % values.size()]; };
}
class FnLLM : public DirectorLLM {
public:
    std::function<DirectorLLMResult(const DirectorLLMArgs&)> fn;
    DirectorLLMResult invoke(const DirectorLLMArgs& a) override { return fn(a); }
};
Json load() {
    auto text = fs::read_file(test::fixtures_dir() + "/discussion/cases.json");
    if (!text) throw std::runtime_error("fixture missing");
    return parse_json_or_throw(*text, {8u << 20, 1'000'000, 64});
}
PartyConfig cfg_from(const Json& j) {
    PartyConfig c;
    c.expectedUtterances = j["expectedUtterances"].get<double>();
    c.keymanCount = j["keymanCount"].get<int>();
    c.facilitatorModel = j["facilitatorModel"].get<std::string>();
    c.keymanModel = j["keymanModel"].get<std::string>();
    for (const auto& m : j["opinionModels"]) c.opinionModels.push_back({m["model"].get<std::string>(), m["weight"].get<double>()});
    c.minTotal = j["minTotal"].get<int>();
    return c;
}
const PartyConfig PARTY{3, 2, "opus", "opus", {{"sonnet", 6}, {"haiku", 4}}, 4};
} // namespace

TEST_CASE("rounds / budget gate: 6 utterances, 2 rounds, stop; swap reflected; summary posted") {
    FnLLM llm;
    int n = 0;
    llm.fn = [&](const DirectorLLMArgs& a) { return DirectorLLMResult{true, "発言" + std::to_string(++n) + " (model=" + a.model + ")", ""}; };
    int asks = 0;
    std::vector<std::string> r1, r2;
    std::string summary;
    DirectorDeps deps;
    deps.llm = &llm;
    deps.postUtterance = [&](const PartyMember& m, const std::string&) { (asks == 0 ? r1 : r2).push_back(m.personaName); };
    deps.askContinue = [&](const ContinueContext&) { return ++asks == 1; };
    deps.postSummary = [&](const PartyMember&, const std::string& t) { summary = t; };
    deps.rng = seq_rng({0.1, 0.5, 0.9, 0.2, 0.7, 0.3, 0.6, 0.4, 0.8, 0.05, 0.15, 0.25});
    deps.maxRounds = 3;
    auto result = run_discussion({"テスト議題", PARTY}, deps);
    CHECK_EQ(result.stoppedBy, std::string("user-stop"));
    CHECK_EQ(result.rounds, 2);
    CHECK_EQ(asks, 2);
    CHECK_EQ(result.utterances.size(), std::size_t(6));
    std::set<std::string> s1(r1.begin(), r1.end());
    int newcomers = 0;
    for (const auto& x : std::set<std::string>(r2.begin(), r2.end())) newcomers += !s1.count(x);
    CHECK(newcomers >= 1);
    CHECK(!summary.empty());
}

TEST_CASE("run_discussion matches Node golden (prompts, posts, logs, failures, max-rounds)") {
    const Json all = load();
    const std::vector<double> seq = {0.1, 0.5, 0.9, 0.2, 0.7, 0.3, 0.6, 0.4, 0.8, 0.05, 0.15, 0.25, 0.99, 0.0, 0.45};
    for (const auto& run : all["runs"]) {
        const auto cfg = cfg_from(all["configs"][run["cfgIndex"].get<std::size_t>()]);
        const int continue_times = run["continueTimes"].get<int>();
        const int fail_every = run["failEvery"].get<int>();
        FnLLM llm;
        Json calls = Json::array(), posted = Json::array(), summary;
        std::vector<std::string> logs;
        int n = 0, asks = 0;
        llm.fn = [&](const DirectorLLMArgs& a) {
            calls.push_back(Json{{"system", a.system}, {"prompt", a.prompt}, {"model", a.model}});
            ++n;
            if (fail_every && n % fail_every == 0)
                return n % 2 ? DirectorLLMResult{false, "", "boom"} : DirectorLLMResult{true, "   ", ""};
            return DirectorLLMResult{true, " 発言" + std::to_string(n) + " ", ""};
        };
        DirectorDeps deps;
        deps.llm = &llm;
        deps.postUtterance = [&](const PartyMember& m, const std::string& t) { posted.push_back(Json::array({m.slotId, m.personaName, t})); };
        deps.askContinue = [&](const ContinueContext& c) {
            ++asks;
            posted.push_back(Json::array({"ask", Json{{"round", c.round}, {"total", c.total}}}));
            return asks <= continue_times;
        };
        deps.postSummary = [&](const PartyMember& m, const std::string& t) { summary = Json::array({m.slotId, t}); };
        deps.log = [&](const std::string& m) { logs.push_back(m); };
        deps.rng = seq_rng(seq);
        deps.maxRounds = run["maxRounds"].get<int>();
        auto result = run_discussion({"テスト議題\n二行目", cfg}, deps);
        CHECK_EQ(json_stringify(calls), json_stringify(run["calls"]));
        CHECK_EQ(json_stringify(posted), json_stringify(run["posted"]));
        CHECK_EQ(json_stringify(Json(logs)), json_stringify(run["logs"]));
        CHECK_EQ(json_stringify(summary), json_stringify(run["summary"]));
        CHECK_EQ(result.rounds, run["result"]["rounds"].get<int>());
        CHECK_EQ(result.stoppedBy, run["result"]["stoppedBy"].get<std::string>());
        CHECK_EQ(result.utterances.size(), run["result"]["utterances"].size());
    }
}

TEST_CASE("persistent LLM failure no longer loops forever (round aborted after consecutive skips)") {
    FnLLM llm;
    int calls = 0;
    llm.fn = [&](const DirectorLLMArgs&) {
        ++calls;
        return DirectorLLMResult{false, "", "down"};
    };
    DirectorDeps deps;
    deps.llm = &llm;
    deps.askContinue = [](const ContinueContext&) { return false; };
    deps.rng = seq_rng({0.3});
    auto result = run_discussion({"t", PARTY}, deps);
    CHECK_EQ(calls, kMaxConsecutiveSkips);
    CHECK(result.utterances.empty());
    CHECK_EQ(result.stoppedBy, std::string("user-stop"));
    DirectorDeps missing;
    CHECK_THROWS(run_discussion({"t", PARTY}, missing));
}

DI_TEST_MAIN()
