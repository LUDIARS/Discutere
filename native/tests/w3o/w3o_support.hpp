#pragma once
// w3-O のテストが共有する補助: golden fixture (w3o/golden.json)、偽の LLM、JSON の比較、一時ディレクトリ。
#include "check.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "persona_engine/llm/client.hpp"

namespace w3o {

using di::Json;

inline const Json& golden() {
    static const Json doc = [] {
        auto text = di::fs::read_file(di::test::fixtures_dir() + "/w3o/golden.json", 64u << 20);
        if (!text) throw di::test::Failure("fixture missing: w3o/golden.json");
        return di::parse_json_or_throw(*text, {64u << 20, 5'000'000, 64});
    }();
    return doc;
}

/** キーの順序を無視した比較 (数値は値で比べる)。 */
inline bool same(const Json& a, const Json& b) { return nlohmann::json::parse(a.dump()) == nlohmann::json::parse(b.dump()); }

#define W3O_SAME(actual, expected)                                                                                       \
    do {                                                                                                                 \
        const ::di::Json w3o_a = (actual);                                                                               \
        const ::di::Json w3o_e = (expected);                                                                             \
        if (!::w3o::same(w3o_a, w3o_e))                                                                                  \
            throw ::di::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #actual "\n  actual:   " + \
                                      w3o_a.dump() + "\n  expected: " + w3o_e.dump());                                  \
    } while (0)

/** 決まった応答を順に返す偽の LLM (最後の応答を繰り返す)。{"throws": msg} は例外を投げる。呼び出しを記録する。 */
class FakeLLM : public di::persona_engine::LLMClient {
public:
    explicit FakeLLM(Json responses) : responses_(std::move(responses)) {}
    di::persona_engine::LLMResult invoke(const di::persona_engine::LLMInvokeArgs& args) override {
        std::lock_guard lock(mutex_);
        Json call = Json::object();
        call["system"] = args.system ? Json(*args.system) : Json(nullptr);
        call["prompt"] = args.prompt;
        call["model"] = args.model ? Json(*args.model) : Json(nullptr);
        call["maxTokens"] = args.maxTokens ? Json(*args.maxTokens) : Json(nullptr);
        calls.push_back(call);
        if (!responses_.is_array() || responses_.empty()) return di::persona_engine::LLMResult::failure("no response");
        const Json& r = responses_[std::min(index_, responses_.size() - 1)];
        ++index_;
        if (r.contains("throws")) throw std::runtime_error(r["throws"].get<std::string>());
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

/** 一時ディレクトリ (終了時に消す)。 */
struct TempDir {
    std::string path;
    explicit TempDir(const std::string& prefix = "w3o-") {
        path = (std::filesystem::temp_directory_path() / (prefix + di::crypto::uuid_v4())).string();
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

inline std::vector<std::string> strings(const Json& a) {
    std::vector<std::string> out;
    if (a.is_array())
        for (const auto& s : a) out.push_back(s.get<std::string>());
    return out;
}

} // namespace w3o
