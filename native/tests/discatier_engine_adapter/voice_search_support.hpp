#pragma once
// voice_search_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w3-N: 外部の声の検索 (voice-search)・adapter の外部の声 (仮名化・出所)・external-voices の配線・機密タグの遮断。
// tests/persona-engine/external-voices.test.ts, external-voices-lookup.test.ts の移植 + golden (Node と順位が一致) + 境界。
#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "w3n_support.hpp"

#include "base/strings.hpp"
#include "base/time.hpp"
#include "core/vectors/embedder.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/persona.hpp"
#include "discatier_engine_adapter/index.hpp"
#include "discatier_engine_adapter/voice_search.hpp"
#include "flow/external_voices.hpp"
#include "flow/tags.hpp"

using namespace di;
using namespace di::discatier_engine_adapter;

namespace voice_search_support {

class StubEmbedder : public core::vectors::EmbeddingClient {
public:
    explicit StubEmbedder(std::vector<double> v) : v_(std::move(v)) {}
    std::vector<std::vector<double>> embed(const std::vector<std::string>& texts) override {
        calls += 1;
        return std::vector<std::vector<double>>(texts.size(), v_);
    }
    std::atomic<int> calls{0};

private:
    std::vector<double> v_;
};

std::vector<std::string> strings_of(const Json& arr) {
    std::vector<std::string> out;
    for (const auto& x : arr) out.push_back(x.get<std::string>());
    return out;
}

/** golden の DB を組む (attribution は KG ディレクトリの sidecar)。 */
std::unique_ptr<core::Core> golden_core(const w3n::TempDir& dir, const Json& fx) {
    auto core = core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
    w3n::load_tables(*core, fx["tables"]);
    auto store = crawler::sources::open_attribution_store();
    for (const auto& a : fx["attributions"]) {
        crawler::sources::AttributionInput in;
        in.utteranceId = a["utteranceId"].get<std::string>();
        in.source = a["source"].get<std::string>();
        in.sourceUrl = a["sourceUrl"].get<std::string>();
        in.nativeId = a["nativeId"].get<std::string>();
        if (a["gameSlug"].is_string()) in.gameSlug = a["gameSlug"].get<std::string>();
        in.at = a["at"].get<double>();
        store->set(in);
    }
    store->close();
    return core;
}

core::repositories::UtteranceInput utt(const std::string& id, const std::string& speaker, const std::string& text, double posted) {
    core::repositories::UtteranceInput u;
    u.id = id;
    u.workspaceId = "knowledge";
    u.sessionId = "ext:s";
    u.speakerId = speaker;
    u.rawContent = text;
    u.postedAt = posted;
    return u;
}

} // namespace voice_search_support
