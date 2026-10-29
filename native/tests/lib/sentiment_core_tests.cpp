// @ludiars/sentiment-core との互換 (golden: native/tests/golden/w1-E-sentiment_core.gen.ts が記録した Node の出力)
// と、元の TS テスト (lib/lapilli/packages/sentiment-core/tests/vector.test.ts, tests/sentiment-core/golden.test.ts) の観点。
#include "check.hpp"

#include <cmath>
#include <cstdlib>

#include "analysis/affect_score.hpp"
#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "crawler/sentiment/cascade.hpp"
#include "flow/sentiment_vector.hpp"
#include "lib/sentiment_core/sentiment_core.hpp"

using namespace di;
namespace sc = di::lib::sentiment_core;

namespace {

Json load(const std::string& name) {
    auto text = fs::read_file(test::fixtures_dir() + "/sentiment_core/" + name);
    if (!text) throw test::Failure("fixture missing: " + name);
    return parse_json_or_throw(*text, {64u << 20, 5'000'000, 64});
}

std::string repo_root() {
    const char* r = std::getenv("DI_REPO_ROOT");
    return r ? r : ".";
}

std::optional<sc::Signal> signal_from(const Json& j) {
    if (j.is_null()) return std::nullopt;
    sc::Signal s;
    if (j.contains("votedUp")) s.votedUp = j["votedUp"].get<bool>();
    if (j.contains("upvotes")) s.upvotes = j["upvotes"].get<double>();
    return s;
}

class Recorded : public sc::SentimentLlmClient {
public:
    Recorded(std::string text, std::vector<std::string>* prompts) : text_(std::move(text)), prompts_(prompts) {}
    sc::SentimentLlmResponse invoke(const sc::SentimentLlmRequest& r) override {
        Json j = Json::object();
        j["system"] = r.system;
        j["prompt"] = r.prompt;
        j["maxTokens"] = r.maxTokens;
        prompts_->push_back(json_stringify(j));
        return {true, text_, {}};
    }

private:
    std::string text_;
    std::vector<std::string>* prompts_;
};

// JSON.stringify と同じく NaN は null として比べる。
Json result_json(const sc::SentimentResult& r) {
    Json j = sc::to_json(r);
    if (std::isnan(r.confidence)) j["confidence"] = nullptr;
    if (std::isnan(r.score)) j["score"] = nullptr;
    return j;
}

} // namespace

TEST_CASE("embedded dictionaries equal the package data files") {
    const std::string base = repo_root() + "/lib/lapilli/packages/sentiment-core/data/";
    auto lex = fs::read_file(base + "lexicon.json");
    auto voc = fs::read_file(base + "vocabulary.json");
    if (!lex || !voc) {
        std::cout << "  (sentiment-core package not checked out; skip)\n";
        return;
    }
    CHECK(*lex == std::string(sc::embedded_lexicon_json()));
    CHECK(*voc == std::string(sc::embedded_vocabulary_json()));
}

TEST_CASE("fixed 20-dimensional vector (vector.test.ts)") {
    CHECK_EQ(sc::DIM, std::size_t{20});
    CHECK_EQ(sc::vector_spec().size(), std::size_t{20});
    CHECK_EQ(sc::text_to_vector("最高に面白い!").size(), std::size_t{20});
    CHECK(sc::text_to_vector("最高に面白い! story も beautiful") ==
          sc::build_vector({sc::extract_text_features("最高に面白い! story も beautiful")}));
    CHECK_EQ(sc::score_text("最高に面白い!").valenceLabel, std::string("positive"));
    const auto target = sc::build_target_vector({sc::MechanicIntent{std::vector<std::string>{"fun"}, std::vector<std::string>{"joy"}, "positive"}});
    CHECK_EQ(target.size(), sc::DIM);
    CHECK_EQ(sc::compute_design_gap(sc::Vector(sc::DIM, 0.5), target).size(), sc::DIM);
    CHECK(sc::weighted_mean({sc::Vector(sc::DIM, 0), sc::Vector(sc::DIM, 1)}, {1, 3}) == sc::Vector(sc::DIM, 0.75));
    const auto vocab = sc::load_affect_vocabulary();
    CHECK_EQ(vocab.size(), std::size_t{24});
    bool found = false;
    for (const auto& e : vocab) found = found || e.key == "tension_release";
    CHECK(found);
}

TEST_CASE("text features, vectors and scores match Node") {
    const Json doc = load("text.json");
    CHECK_EQ(json_stringify(doc["spec"]), json_stringify(Json(sc::vector_spec())));
    for (const auto& c : doc["cases"]) {
        const std::string text = c["text"].get<std::string>();
        CHECK_EQ(json_stringify(sc::vector_to_json(sc::text_to_vector(text))), json_stringify(c["vector"]));
        CHECK_EQ(json_stringify(sc::to_json(sc::score_text(text))), json_stringify(c["score"]));
        // 旧 import 先 (analysis / flow) も同じ実体
        CHECK_EQ(json_stringify(sc::to_json(analysis::score_text(text))), json_stringify(c["score"]));
        CHECK(flow::text_to_vector(text) == sc::text_to_vector(text));
    }
}

TEST_CASE("golden digests of tests/sentiment-core/golden.json") {
    auto golden_text = fs::read_file(repo_root() + "/tests/sentiment-core/golden.json");
    if (!golden_text) {
        std::cout << "  (golden.json not found; skip)\n";
        return;
    }
    const Json golden = parse_json_or_throw(*golden_text);
    const Json text = load("text.json");
    const Json misc = load("misc.json");
    const std::size_t n = misc["goldenTextCount"].get<std::size_t>();
    Json vectors = Json::array(), scores = Json::array(), cascades = Json::array();
    for (std::size_t i = 0; i < n; ++i) {
        const std::string t = text["cases"][i]["text"].get<std::string>();
        vectors.push_back(sc::vector_to_json(sc::text_to_vector(t)));
        scores.push_back(sc::to_json(sc::score_text(t)));
        cascades.push_back(sc::to_json(crawler::sentiment::cascade_sentiment(t, std::nullopt, {})));
    }
    CHECK_EQ(crypto::sha256_hex(json_stringify(vectors)), golden["textToVector"].get<std::string>());
    CHECK_EQ(crypto::sha256_hex(json_stringify(scores)), golden["scoreText"].get<std::string>());
    CHECK_EQ(crypto::sha256_hex(json_stringify(cascades)), golden["cascadeTier0"].get<std::string>());
    // 集計ベクトル (aggregate-input.json)
    auto agg_text = fs::read_file(repo_root() + "/tests/sentiment-core/aggregate-input.json");
    CHECK(agg_text.has_value());
    const Json agg = parse_json_or_throw(*agg_text);
    std::vector<sc::TextFeatures> features;
    for (const auto& r : agg["records"]) {
        sc::SentimentSignal s;
        const Json& meta = json_object(r, "meta");
        if (meta.contains("voted_up") && meta["voted_up"].is_boolean()) s.votedUp = meta["voted_up"].get<bool>();
        features.push_back(sc::extract_text_features(r["text"].get<std::string>(), s));
    }
    CHECK_EQ(json_stringify(sc::vector_to_json(sc::build_vector(features))), json_stringify(golden["gameVector"]));
    CHECK_EQ(json_stringify(sc::vector_to_json(sc::build_vector(features))), json_stringify(misc["gameVector"]));
}

TEST_CASE("cascade tier 0 with signals matches Node") {
    const Json doc = load("cascade.json");
    for (const auto& c : doc["tier0"]) {
        const auto r = sc::cascade_sentiment(c["text"].get<std::string>(), signal_from(c["signal"]), {});
        CHECK_EQ(json_stringify(result_json(r)), json_stringify(c["result"]));
    }
}

TEST_CASE("cascade LLM tiers with recorded responses match Node") {
    const Json doc = load("cascade.json");
    for (const auto& c : doc["llm"]) {
        std::vector<std::string> prompts;
        sc::CascadeClients clients;
        if (!c["local"].is_null()) clients.local = std::make_shared<Recorded>(c["local"].get<std::string>(), &prompts);
        if (!c["main"].is_null()) clients.main = std::make_shared<Recorded>(c["main"].get<std::string>(), &prompts);
        const auto r = sc::cascade_sentiment("so-so " + std::string(320, 'x'), std::nullopt, clients);
        CHECK_EQ(json_stringify(result_json(r)), json_stringify(c["result"]));
        CHECK_EQ(json_stringify(Json(prompts)), json_stringify(c["prompts"]));
    }
}

TEST_CASE("LLM response parsing bounds") {
    CHECK(!sc::parse_sentiment_response("").has_value());
    CHECK(!sc::parse_sentiment_response("}{").has_value());
    // 上限ちょうどは読める、上限 + 1 は読まない
    const std::string head = "{\"polarity\":\"positive\",\"confidence\":0.9,\"pad\":\"";
    const std::string tail = "\"}";
    std::string at_limit = head + std::string(sc::kMaxSentimentResponseBytes - head.size() - tail.size(), 'a') + tail;
    CHECK_EQ(at_limit.size(), sc::kMaxSentimentResponseBytes);
    CHECK(sc::parse_sentiment_response(at_limit).has_value());
    std::string over = head + std::string(sc::kMaxSentimentResponseBytes - head.size() - tail.size() + 1, 'a') + tail;
    CHECK(!sc::parse_sentiment_response(over).has_value());
    // 巨大・深い入れ子は拒否 (落ちない)
    CHECK(!sc::parse_sentiment_response(std::string(10'000, '{') + std::string(10'000, '}')).has_value());
    CHECK(!sc::parse_sentiment_response(std::string(4u << 20, '{') + "}").has_value());
}

TEST_CASE("design gap, speaker profiles, math and vocabulary match Node") {
    const Json doc = load("misc.json");
    for (const auto& t : doc["targets"]) {
        std::vector<sc::MechanicIntent> mechanics;
        for (const auto& m : t["mechanics"]) {
            sc::MechanicIntent mi;
            if (m.contains("intended_aspects")) mi.intended_aspects = m["intended_aspects"].get<std::vector<std::string>>();
            if (m.contains("intended_emotions")) mi.intended_emotions = m["intended_emotions"].get<std::vector<std::string>>();
            if (m.contains("intended_valence")) mi.intended_valence = m["intended_valence"].get<std::string>();
            mechanics.push_back(std::move(mi));
        }
        const auto target = sc::build_target_vector(mechanics);
        CHECK_EQ(json_stringify(sc::vector_to_json(target)), json_stringify(t["target"]));
        CHECK_EQ(json_stringify(sc::vector_to_json(sc::compute_design_gap(sc::text_to_vector("最高に面白い!"), target))),
                 json_stringify(t["gap"]));
    }
    std::vector<sc::SpeakerOpinions> speakers;
    for (const auto& s : doc["speakers"]["input"]) {
        sc::SpeakerOpinions so;
        so.speakerId = s["speakerId"].get<std::string>();
        so.source = json_opt_string(s, "source");
        for (const auto& o : s["opinions"]) {
            sc::SpeakerOpinion op;
            op.text = o["text"].get<std::string>();
            op.gameSlug = json_opt_string(o, "gameSlug");
            op.weight = json_opt_number(o, "weight");
            so.opinions.push_back(std::move(op));
        }
        speakers.push_back(std::move(so));
    }
    CHECK_EQ(json_stringify(sc::to_json(sc::evaluate_speakers(speakers))), json_stringify(doc["speakers"]["defaults"]));
    CHECK_EQ(json_stringify(sc::to_json(sc::evaluate_speakers(speakers, {2.0, 0.2, 0.5}))),
             json_stringify(doc["speakers"]["custom"]));

    const auto& math = doc["math"];
    const auto left = math["left"].get<std::vector<double>>();
    const auto right = math["right"].get<std::vector<double>>();
    CHECK_EQ(json_stringify(Json(sc::cosine(left, right))), json_stringify(math["cosine"]));
    CHECK_EQ(json_stringify(Json(sc::scalar_projection(left, right))), json_stringify(math["projection"]));
    CHECK_EQ(json_stringify(sc::vector_to_json(sc::weighted_mean({left, right}, {1, 3}))), json_stringify(math["mean"]));
    CHECK_THROWS(sc::dot({1, 2}, {1}));
    CHECK_THROWS(sc::weighted_mean({{1}}, {}));
    CHECK_THROWS(sc::weighted_mean({{1}, {1, 2}}, {1, 1}));
    CHECK(sc::weighted_mean({}, {}) == sc::Vector(sc::DIM, 0.0));

    CHECK_EQ(sc::affect_vocabulary_version(), doc["vocabulary"]["version"].get<std::string>());
    Json entries = Json::array();
    for (const auto& e : sc::load_affect_vocabulary()) entries.push_back(sc::to_json(e));
    CHECK_EQ(json_stringify(entries), json_stringify(doc["vocabulary"]["entries"]));
}

TEST_CASE("js_to_fixed follows Number#toFixed") {
    CHECK_EQ(sc::js_to_fixed(1.03125, 4), std::string("1.0313"));  // 同点は大きい方
    CHECK_EQ(sc::js_to_fixed(0.5, 0), std::string("1"));
    CHECK_EQ(sc::js_to_fixed(2.5, 0), std::string("3"));
    CHECK_EQ(sc::js_to_fixed(1.005, 2), std::string("1.00"));  // 1.005 は 1.00499... なので切り捨て
    CHECK_EQ(sc::js_to_fixed(-0.0, 2), std::string("0.00"));
    CHECK_EQ(sc::js_to_fixed(-1.5, 0), std::string("-2"));
    CHECK_EQ(sc::js_to_fixed(9.99995, 4), std::string("10.0000"));
    CHECK_EQ(sc::js_to_fixed(0.12345678, 4), std::string("0.1235"));
    CHECK_EQ(sc::js_to_fixed(123456, 2), std::string("123456.00"));
    CHECK_EQ(sc::js_to_fixed(1e21, 2), std::string("1e+21"));
    CHECK_EQ(sc::js_to_fixed(std::nan(""), 2), std::string("NaN"));
}

DI_TEST_MAIN()
