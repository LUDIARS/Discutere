#pragma once
// w2-G のテストが共有する補助: golden fixture (flow/w2g_golden.json)、偽の LLM、JSON の比較、型の変換。
#include "check.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "config.hpp"
#include "flow/db/connection.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/discussion_paper.hpp"
#include "persona_engine/llm/client.hpp"

namespace w2g {

using di::Json;

inline const Json& golden() {
    static const Json doc = [] {
        auto text = di::fs::read_file(di::test::fixtures_dir() + "/flow/w2g_golden.json", 64u << 20);
        if (!text) throw di::test::Failure("fixture missing: flow/w2g_golden.json");
        return di::parse_json_or_throw(*text, {64u << 20, 5'000'000, 64});
    }();
    return doc;
}

/** キーの順序を無視した比較 (数値は値で比べる)。 */
inline bool same(const Json& a, const Json& b) { return nlohmann::json::parse(a.dump()) == nlohmann::json::parse(b.dump()); }

#define W2G_SAME(actual, expected)                                                                                       \
    do {                                                                                                                 \
        const ::di::Json w2g_a = (actual);                                                                               \
        const ::di::Json w2g_e = (expected);                                                                             \
        if (!::w2g::same(w2g_a, w2g_e))                                                                                  \
            throw ::di::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #actual "\n  actual:   " + \
                                      w2g_a.dump() + "\n  expected: " + w2g_e.dump());                                  \
    } while (0)

/** 決まった応答を順に返す偽の LLM (最後の応答を繰り返す)。呼び出しを記録する。 */
class FakeLLM : public di::persona_engine::LLMClient {
public:
    explicit FakeLLM(const Json& responses) : responses_(responses) {}
    di::persona_engine::LLMResult invoke(const di::persona_engine::LLMInvokeArgs& args) override {
        std::lock_guard lock(mutex_);
        Json call = Json::object();
        call["system"] = args.system ? Json(*args.system) : Json(nullptr);
        call["prompt"] = args.prompt;
        call["model"] = args.model ? Json(*args.model) : Json(nullptr);
        call["maxTokens"] = args.maxTokens ? Json(*args.maxTokens) : Json(nullptr);
        calls.push_back(call);
        if (responses_.empty()) return di::persona_engine::LLMResult::failure("no response");
        const Json& r = responses_[std::min(index_, responses_.size() - 1)];
        ++index_;
        if (r.value("ok", false)) return di::persona_engine::LLMResult::success(r.value("text", ""));
        return di::persona_engine::LLMResult::failure(r.value("error", ""));
    }
    Json calls = Json::array();

private:
    Json responses_;
    std::size_t index_ = 0;
    std::mutex mutex_;
};

struct Warns {
    std::vector<std::string> list;
    std::function<void(const std::string&)> fn() {
        return [this](const std::string& m) { list.push_back(m); };
    }
    Json json() const { return Json(list); }
};

inline std::optional<std::string> opt(const Json& o, const char* key) {
    if (!o.is_object() || !o.contains(key) || o[key].is_null()) return std::nullopt;
    return o[key].get<std::string>();
}

inline std::vector<std::string> strings(const Json& a) {
    std::vector<std::string> out;
    if (a.is_array())
        for (const auto& s : a) out.push_back(s.get<std::string>());
    return out;
}

inline di::flow::dialectic::IssueRecord issue_from(const Json& j) {
    return {j["id"], j["sessionId"], j["title"], j["ordinal"].get<int>(), j["source"], j["status"]};
}

inline di::flow::dialectic::PositionRecord position_from(const Json& j) {
    di::flow::dialectic::PositionRecord p;
    p.id = j["id"];
    p.issueId = j["issueId"];
    p.personaId = j["personaId"];
    p.stance = j["stance"];
    p.claim = j["claim"];
    p.qualifier = opt(j, "qualifier");
    for (const auto& g : j["grounds"]) p.grounds.push_back(di::flow::dialectic::ground_from_json(g));
    p.values = strings(j["values"]);
    return p;
}

inline di::flow::dialectic::TensionRecord tension_from(const Json& j) {
    return {j["id"], j["issueId"], j["positionA"], j["positionB"], j["type"], j["status"], opt(j, "resolutionNote")};
}

inline std::vector<di::flow::ContextVoice> voices_from(const Json& a) {
    std::vector<di::flow::ContextVoice> out;
    for (const auto& v : a) out.push_back(di::flow::context_voice_from_json(v));
    return out;
}

inline Json voices_json(const std::vector<di::flow::ContextVoice>& v) {
    Json a = Json::array();
    for (const auto& x : v) a.push_back(di::flow::to_json(x));
    return a;
}

/** テスト用の flow DB を一時ファイルに作る (DATABASE_PATH を差し替え、本物の data/ を読まない)。 */
class TempFlowDb {
public:
    TempFlowDb() {
        dir = (std::filesystem::temp_directory_path() / ("w2g-" + di::crypto::uuid_v4())).string();
        std::filesystem::create_directories(dir);
        di::env::set_for_test("DATABASE_PATH", std::string_view(dir + "/flow.db"));
        di::env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir + "/no-config.json"));
        di::config::reset_config_for_test();
        di::flow::db::reset_flow_db_for_test();
    }
    ~TempFlowDb() {
        di::flow::db::reset_flow_db_for_test();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempFlowDb(const TempFlowDb&) = delete;
    TempFlowDb& operator=(const TempFlowDb&) = delete;
    std::string dir;
};

} // namespace w2g
