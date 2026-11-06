#pragma once
// w3k_user_voices_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w3-K: ユーザーの声 (tests/flow/user-voices.test.ts の collect / listGameVoices / parseSimilarGames 部分、
// impression-bridge.test.ts の「届いていた感想への fallback」)、importer (tests/crawler/external/importer.test.ts、
// summary.test.ts の importer 部分)、voice-index (部分ベクトル化)、prepare_user_voices。
#include <filesystem>

#include "check.hpp"
#include "core/test_util.hpp"

#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/vectors/voice_index.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/importer.hpp"
#include "crawler/sources/ingested_store.hpp"
#include "crawler/sources/raw_store.hpp"
#include "flow/user_voices/collect.hpp"
#include "flow/user_voices/game_voices.hpp"
#include "flow/user_voices/runtime.hpp"
#include "flow/user_voices/voluptas_impressions.hpp"

using namespace di;
using namespace di::flow::user_voices;
using di::flow::ContextVoice;
namespace cs = di::crawler::sources;

namespace w3k_user_voices_support {

cs::ExternalUtterance utt(std::string source, std::string native_id, std::string slug, std::string thread, std::string content, double at,
                          std::string author, std::string url = "u") {
    cs::ExternalUtterance u;
    u.source = std::move(source);
    u.nativeId = std::move(native_id);
    u.gameSlug = std::move(slug);
    u.threadKey = std::move(thread);
    u.content = std::move(content);
    u.postedAt = at;
    u.authorId = std::move(author);
    u.sourceUrl = std::move(url);
    return u;
}

/** 呼ばれた id を覚える偽の埋め込み (2 次元)。 */
class FakeEmbedder : public core::vectors::EmbeddingClient {
public:
    std::vector<std::vector<std::string>> batches;
    bool short_reply = false;
    std::vector<std::vector<double>> embed(const std::vector<std::string>& texts) override {
        batches.push_back(texts);
        std::vector<std::vector<double>> out;
        for (std::size_t i = 0; i < texts.size(); ++i) out.push_back({1.0, static_cast<double>(i)});
        if (short_reply && !out.empty()) out.pop_back();
        return out;
    }
};

CollectUserVoicesDeps base_deps(std::vector<std::vector<std::string>>* embedded) {
    CollectUserVoicesDeps d;
    d.workspaceId = "knowledge";
    d.fetchSteam = [](const SteamApp&, const std::string& slug) {
        return std::vector<cs::ExternalUtterance>{utt("steam", "r1", slug, "1", "面白い", 1, "S")};
    };
    d.fetchGlab = [](const GameRef&, const std::string& slug) {
        return std::vector<cs::ExternalUtterance>{utt("glab", "v1", slug, "新作", "重い", 1, "anonymous", "g")};
    };
    d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>& items, const std::string&) { return static_cast<int>(items.size()); };
    d.embed = [embedded](core::Core*, const std::vector<std::string>& ids) {
        embedded->push_back(ids);
        return static_cast<int>(ids.size());
    };
    return d;
}

} // namespace w3k_user_voices_support
