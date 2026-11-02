#pragma once
// w2-I のテスト共通の補助 (fixture・一時 DB・記録する ContextProvider / Logger)。
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "check.hpp"
#include "persona_engine/context_provider.hpp"
#include "persona_engine/types.hpp"

namespace w2i {

inline di::Json load_fixture(const std::string& name) {
    auto text = di::fs::read_file(di::test::fixtures_dir() + "/w2i/" + name, 32u << 20);
    if (!text) throw std::runtime_error("fixture missing: " + name);
    di::JsonLimits limits;
    limits.max_bytes = 32u << 20;
    limits.max_elements = 2'000'000;
    return di::parse_json_or_throw(*text, limits);
}

inline std::string temp_dir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / ("di-w2i-" + name + "-" + di::crypto::uuid_v4());
    std::filesystem::create_directories(dir);
    return di::fs::from_path(dir);
}

inline di::sql::DatabasePtr temp_db(const std::string& name) { return di::sql::Database::open(di::fs::join(temp_dir(name), "pe.db")); }

/** 値を記録する ContextProvider (TS テストの in-memory ctx)。 */
class RecordingProvider : public di::persona_engine::DiscussionContextProvider {
public:
    std::vector<di::persona_engine::ContextGap> gaps;
    std::vector<di::persona_engine::ContextHypothesis> hypotheses;
    std::vector<di::persona_engine::ProposeHypothesisInput> proposed;
    std::vector<di::persona_engine::PostUtteranceInput> posted;
    std::vector<std::pair<std::string, di::Json>> findCalls;
    bool hasFind = false, hasActive = false;
    std::optional<std::string> findResult, activeResult;
    mutable std::mutex mutex;

    std::vector<di::persona_engine::ContextHypothesis> list_active_hypotheses(const std::string&, int) override { return hypotheses; }
    std::vector<di::persona_engine::ContextGap> list_recent_gaps(const std::string&, int) override { return gaps; }
    std::vector<di::persona_engine::ContextUtterance> list_recent_utterances(const std::string&, const std::string&, int) override {
        return {};
    }
    std::string propose_hypothesis(const di::persona_engine::ProposeHypothesisInput& input) override {
        std::lock_guard lock(mutex);
        proposed.push_back(input);
        return "h-" + std::to_string(proposed.size());
    }
    std::string post_utterance(const di::persona_engine::PostUtteranceInput& input) override {
        std::lock_guard lock(mutex);
        posted.push_back(input);
        return "u-" + std::to_string(posted.size());
    }
    std::optional<std::string> find_discussion_session(const di::persona_engine::FindDiscussionSessionInput& q) override {
        if (!hasFind) return std::nullopt;
        di::Json j = di::Json::object();
        j["workspaceId"] = q.workspaceId;
        if (q.hypothesisId) j["hypothesisId"] = *q.hypothesisId;
        findCalls.emplace_back("find", j);
        return findResult;
    }
    std::optional<std::string> find_active_discussion_session(const std::string& ws) override {
        if (!hasActive) return std::nullopt;
        findCalls.emplace_back("active", di::Json{{"workspaceId", ws}});
        return activeResult;
    }
};

class RecordingLogger : public di::persona_engine::Logger {
public:
    std::vector<di::Json> calls;
    std::mutex mutex;
    void debug(const di::Json&, std::string_view) override {}
    void info(const di::Json& m, std::string_view msg) override { push("info", m, msg); }
    void warn(const di::Json& m, std::string_view msg) override { push("warn", m, msg); }
    void error(const di::Json& m, std::string_view msg) override { push("error", m, msg); }

private:
    void push(const char* level, const di::Json& m, std::string_view msg) {
        std::lock_guard lock(mutex);
        calls.push_back(di::Json{{"level", level}, {"meta", m}, {"msg", std::string(msg)}});
    }
};

} // namespace w2i
