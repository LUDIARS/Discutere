#pragma once
// w3-L のテストが共有する補助: golden fixture (flow/w3l_golden.json)、UUID の正規化、記録した応答を返す偽の LLM、
// 一時 flow DB、DB の全行の書き出し。
#include "check.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "config.hpp"
#include "flow/db/connection.hpp"
#include "persona_engine/llm/client.hpp"

namespace w3l {

using di::Json;

inline const Json& golden() {
    static const Json doc = [] {
        auto text = di::fs::read_file(di::test::fixtures_dir() + "/flow/w3l_golden.json", 64u << 20);
        if (!text) throw di::test::Failure("fixture missing: flow/w3l_golden.json");
        return di::parse_json_or_throw(*text, {64u << 20, 5'000'000, 64});
    }();
    return doc;
}

/** キーの順序を無視した比較 (数値は値で比べる)。 */
inline bool same(const Json& a, const Json& b) { return nlohmann::json::parse(a.dump()) == nlohmann::json::parse(b.dump()); }

#define W3L_SAME(actual, expected)                                                                                         \
    do {                                                                                                                   \
        const ::di::Json w3l_a = (actual);                                                                                 \
        const ::di::Json w3l_e = (expected);                                                                               \
        if (!::w3l::same(w3l_a, w3l_e))                                                                                    \
            throw ::di::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #actual "\n  actual:   " + \
                                      w3l_a.dump() + "\n  expected: " + w3l_e.dump());                                    \
    } while (0)

inline bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

/** s[i..] が UUID v4 (小文字) か。 */
inline bool uuid_at(const std::string& s, std::size_t i) {
    if (i + 36 > s.size()) return false;
    for (std::size_t k = 0; k < 36; ++k) {
        const char c = s[i + k];
        if (k == 8 || k == 13 || k == 18 || k == 23) {
            if (c != '-') return false;
        } else if (!is_hex(c)) {
            return false;
        }
    }
    if (s[i + 14] != '4') return false;
    const char v = s[i + 19];
    return v == '8' || v == '9' || v == 'a' || v == 'b';
}

/** UUID を出てきた順に <U1> <U2> ... へ置き換える (gen.ts の Normalizer と同じ)。 */
class Normalizer {
public:
    std::string str(const std::string& s) {
        std::string out;
        std::size_t i = 0;
        while (i < s.size()) {
            if (uuid_at(s, i)) {
                const std::string id = s.substr(i, 36);
                auto it = map_.find(id);
                if (it == map_.end()) {
                    const std::string ph = "<U" + std::to_string(map_.size() + 1) + ">";
                    it = map_.emplace(id, ph).first;
                    inverse_[ph] = id;
                }
                out += it->second;
                i += 36;
            } else {
                out += s[i++];
            }
        }
        return out;
    }
    Json any(const Json& v) {
        if (v.is_string()) return str(v.get<std::string>());
        if (v.is_array()) {
            Json a = Json::array();
            for (const auto& x : v) a.push_back(any(x));
            return a;
        }
        if (v.is_object()) {
            Json o = Json::object();
            for (const auto& [k, x] : v.items()) {
                const std::string key = str(k);
                o[key] = any(x);
            }
            return o;
        }
        return v;
    }
    /** <Uk> を実際の UUID に戻す (記録した応答を C++ 側の id で返すため)。 */
    std::string restore(const std::string& s) const {
        std::string out;
        std::size_t i = 0;
        while (i < s.size()) {
            if (s.compare(i, 2, "<U") == 0) {
                const auto close = s.find('>', i);
                if (close != std::string::npos) {
                    const auto it = inverse_.find(s.substr(i, close - i + 1));
                    if (it != inverse_.end()) {
                        out += it->second;
                        i = close + 1;
                        continue;
                    }
                }
            }
            out += s[i++];
        }
        return out;
    }

private:
    std::map<std::string, std::string> map_;
    std::map<std::string, std::string> inverse_;
};

/** gen.ts の lcg と同じ乱数 (JS の (s * 1664525 + 1013904223) & 0xffffffff を 32bit の wrap で)。 */
inline std::function<double()> lcg(std::uint32_t seed) {
    auto s = std::make_shared<std::uint32_t>(seed);
    return [s] {
        *s = *s * 1664525u + 1013904223u;
        return static_cast<double>(*s) / 4294967296.0;
    };
}

/** 記録した呼び出し列の応答を順に返す偽の LLM。呼び出しは正規化して記録する。 */
class ReplayLLM : public di::persona_engine::LLMClient {
public:
    ReplayLLM(const Json& recorded, Normalizer& norm) : recorded_(recorded), norm_(norm) {}
    di::persona_engine::LLMResult invoke(const di::persona_engine::LLMInvokeArgs& args) override {
        std::lock_guard lock(mutex_);
        Json call = Json::object();
        call["system"] = args.system ? Json(norm_.str(*args.system)) : Json(nullptr);
        call["prompt"] = norm_.str(args.prompt);
        call["model"] = args.model ? Json(*args.model) : Json(nullptr);
        call["maxTokens"] = args.maxTokens ? Json(*args.maxTokens) : Json(nullptr);
        if (index_ >= recorded_.size()) {
            calls.push_back(call);
            return di::persona_engine::LLMResult::failure("replay exhausted");
        }
        const Json& r = recorded_[index_++]["response"];
        call["response"] = r;
        calls.push_back(call);
        if (r.value("ok", false)) return di::persona_engine::LLMResult::success(norm_.restore(r.value("text", "")));
        return di::persona_engine::LLMResult::failure(r.value("error", ""));
    }
    Json calls = Json::array();

private:
    Json recorded_;
    Normalizer& norm_;
    std::size_t index_ = 0;
    std::mutex mutex_;
};

/** 決まった応答を返す偽の LLM (fn に prompt を渡す)。 */
class FnLLM : public di::persona_engine::LLMClient {
public:
    explicit FnLLM(std::function<di::persona_engine::LLMResult(const di::persona_engine::LLMInvokeArgs&)> fn) : fn_(std::move(fn)) {}
    di::persona_engine::LLMResult invoke(const di::persona_engine::LLMInvokeArgs& args) override {
        std::lock_guard lock(mutex_);
        ++count;
        return fn_(args);
    }
    int count = 0;

private:
    std::function<di::persona_engine::LLMResult(const di::persona_engine::LLMInvokeArgs&)> fn_;
    std::mutex mutex_;
};

/** テスト用の flow DB を一時ファイルに作る (DATABASE_PATH を差し替え、本物の data/ を読まない)。 */
class TempFlowDb {
public:
    TempFlowDb() {
        dir = (std::filesystem::temp_directory_path() / ("w3l-" + di::crypto::uuid_v4())).string();
        std::filesystem::create_directories(dir);
        path = dir + "/flow.db";
        di::env::set_for_test("DATABASE_PATH", std::string_view(path));
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
    std::string path;
};

inline bool is_time_column(const std::string& k) {
    auto ends = [&](const std::string& suf) { return k.size() >= suf.size() && k.compare(k.size() - suf.size(), suf.size(), suf) == 0; };
    return ends("_at") || ends("latency_ms");
}

/** gen.ts の dumpDb と同じ: 全表 (空・管理表を除く) を rowid 順に、時刻の列を除いて正規化する。 */
inline Json dump_db(di::sql::Database& db, Normalizer& norm) {
    Json out = Json::object();
    for (const auto& t : db.all("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")) {
        const std::string name = t.str("name");
        if (name == "_flow_migrations" || name == "sqlite_sequence") continue;
        const auto rows = db.all("SELECT * FROM \"" + name + "\" ORDER BY rowid");
        if (rows.empty()) continue;
        Json list = Json::array();
        for (const auto& r : rows) {
            Json o = Json::object();
            for (std::size_t i = 0; i < r.size(); ++i)
                if (!is_time_column(r.name(i))) o[r.name(i)] = r.value(i).to_json();
            list.push_back(norm.any(o));
        }
        out[name] = std::move(list);
    }
    return out;
}

} // namespace w3l
