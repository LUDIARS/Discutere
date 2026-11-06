#pragma once
// w5-R のテスト共通の補助: fixture の読み込み、応答を system の印で決める偽 LLM (呼び出しを記録)、
// 外部の声の偽の検索、時刻の伏せ字。golden 生成 (native/tests/golden/w5-R-golden.gen.ts) と同じ応答表を持つ。
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "check.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace w5r {

using di::Json;

inline Json load_fixture(const std::string& name) {
    auto text = di::fs::read_file(di::test::fixtures_dir() + "/w5r/" + name, 16u << 20);
    if (!text) throw di::test::Failure("fixture missing: " + name);
    return di::parse_json_or_throw(*text, {16u << 20, 2'000'000, 64});
}

/** gen の ROUTES と同じ (順に見て最初に含まれた印の応答)。 */
inline const std::vector<std::pair<std::string, std::string>>& llm_routes() {
    static const std::vector<std::pair<std::string, std::string>> r = {
        {"情報が足りているか", R"({"density":"rich","covered":["肯定","否定"],"gaps":[]})"},
        {"議論設計者", R"(["周回報酬の是非","課金圧の影響"])"},
        {"両方が根拠を張れるか", R"([{"index":1,"armable":"both"},{"index":"2","armable":"pro-only"},{"index":9,"armable":"bogus"}])"},
        {"渡された markdown ブロック", "前置き {\"proposed\":\"# 議題\\n改稿後の議題\",\"rationale\":\"整えた\"} 後置き"},
        {"根拠段落", "  集めた声によると周回は好評。  "},
        {"You review a Japanese discussion paper",
         R"([{"title":" 論点補強 ","reason":"弱い","suggestedChange":"{{具体例}} を追記"},{"title":"","suggestedChange":"x"},"bad",{"title":"t2","suggestedChange":"s2"}])"},
        {"You check whether you have enough game mechanics",
         R"(x {"ok":"no","confidence":"medium","summary":"  概ね把握  ","knownMechanics":["周回"," ","強化",3],"missingQuestions":["報酬量は?"]} y)"},
        {"hypothesis-labeled player feedback", "[]"},
    };
    return r;
}

class FakeLLM : public di::persona_engine::LLMClient {
public:
    di::persona_engine::LLMResult invoke(const di::persona_engine::LLMInvokeArgs& args) override {
        Json c = Json::object();
        c["prompt"] = args.prompt;
        if (args.system) c["system"] = *args.system;
        if (args.model) c["model"] = *args.model;
        if (args.maxTokens) c["maxTokens"] = *args.maxTokens;
        std::lock_guard lock(m_);
        calls_.push_back(c);
        const std::string sys = args.system.value_or("");
        for (const auto& [mark, text] : llm_routes())
            if (sys.find(mark) != std::string::npos) return di::persona_engine::LLMResult::success(text);
        return di::persona_engine::LLMResult::failure("no route");
    }
    Json take_calls() {
        std::lock_guard lock(m_);
        Json out = Json::array();
        for (auto& c : calls_) out.push_back(std::move(c));
        calls_.clear();
        return out;
    }

private:
    std::mutex m_;
    std::vector<Json> calls_;
};

/** gen の VOICES と同じ外部の声。呼び出し (terms, limit) を記録する。 */
struct FakeVoices {
    std::mutex m;
    Json calls = Json::array();
    di::flow::ListExternalVoicesFn fn() {
        return [this](const std::vector<std::string>& terms, int limit) {
            {
                std::lock_guard lock(m);
                calls.push_back(Json{{"terms", terms}, {"limit", limit}});
            }
            std::vector<di::flow::ContextVoice> all = {
                {"周回が楽しい。報酬も十分。", "youtube", std::string("https://example.invalid/1")},
                {"課金しないと厳しい。" + std::string(), "steam", std::nullopt},
                {"テンポが良い", "niconico", std::nullopt},
            };
            std::string longer;
            for (int i = 0; i < 130; ++i) longer += "長";
            all[1].content += longer;
            if (limit < 0) limit = 0;
            if (all.size() > static_cast<std::size_t>(limit)) all.resize(static_cast<std::size_t>(limit));
            return all;
        };
    }
    Json take() {
        std::lock_guard lock(m);
        Json out = calls;
        calls = Json::array();
        return out;
    }
};

/** 時刻の列 (createdAt / updatedAt / appliedAt / created_at / updated_at) で now 相当の値を <ts> に伏せる。 */
inline void mask_times(Json& j) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const auto& k = it.key();
            if ((k == "createdAt" || k == "updatedAt" || k == "appliedAt" || k == "created_at" || k == "updated_at") && it.value().is_number() &&
                it.value().get<double>() > 1e11)
                it.value() = "<ts>";
            else
                mask_times(it.value());
        }
    } else if (j.is_array()) {
        for (auto& v : j) mask_times(v);
    }
}

/** 違いを見つけやすいように、最初に違うパスを返す (同じなら空)。 */
inline std::string first_diff(const Json& a, const Json& b, const std::string& path = "$") {
    if (a.type() != b.type()) {
        if (a.is_number() && b.is_number() && a.get<double>() == b.get<double>()) return "";
        return path + ": " + a.dump().substr(0, 300) + " != " + b.dump().substr(0, 300);
    }
    if (a.is_object()) {
        std::vector<std::string> ka, kb;
        for (auto it = a.begin(); it != a.end(); ++it) ka.push_back(it.key());
        for (auto it = b.begin(); it != b.end(); ++it) kb.push_back(it.key());
        if (ka != kb) return path + ": keys " + Json(ka).dump() + " != " + Json(kb).dump();
        for (const auto& k : ka)
            if (auto d = first_diff(a[k], b[k], path + "." + k); !d.empty()) return d;
        return "";
    }
    if (a.is_array()) {
        if (a.size() != b.size()) return path + ": size " + std::to_string(a.size()) + " != " + std::to_string(b.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            if (auto d = first_diff(a[i], b[i], path + "[" + std::to_string(i) + "]"); !d.empty()) return d;
        return "";
    }
    if (a.is_number()) return a.get<double>() == b.get<double>() ? "" : path + ": " + a.dump() + " != " + b.dump();
    return a == b ? "" : path + ": " + a.dump().substr(0, 300) + " != " + b.dump().substr(0, 300);
}

#define W5R_CHECK_JSON(actual, expected, what)                                                                  \
    do {                                                                                                        \
        const auto w5r_d = ::w5r::first_diff((actual), (expected));                                             \
        if (!w5r_d.empty()) throw ::di::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                                      ": " + std::string(what) + ": " + w5r_d);                  \
    } while (0)

} // namespace w5r
