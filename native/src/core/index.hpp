#pragma once
// core の組み立て (src/core/index.ts の createCore)。
// KuzuClient (中身は SQLite) と EventLog とリポジトリ群・ベクトル操作をまとめた Core を返す。
// Kuzu グラフ (graph) は持たない (常駐サーバは開かない。w1-C port-notes)。
// 後続のモジュールは core.raw() (TS の core.client.raw) で SQLite に直接触れる。
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "core/db/kuzu_client.hpp"
#include "core/events/event_log.hpp"
#include "core/events/event_types.hpp"
#include "core/repositories/base.hpp"
#include "core/vectors/embedding.hpp"
#include "core/vectors/vector_search.hpp"

namespace di::core {

struct CoreOptions {
    /** SQLite index DB パス (省略時は DISCATIER_KUZU_PATH / ./data/discatier.kuzu)。 */
    std::optional<std::string> sqlitePath;
    /** events.jsonl パス (省略時は ./data/events.jsonl)。 */
    std::optional<std::string> eventsPath;
};

class Core {
public:
    explicit Core(const CoreOptions& options = {});
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    db::KuzuClient client;
    events::EventLog eventLog;

    struct Repos {
        explicit Repos(repositories::CoreContext ctx)
            : person(ctx), game(ctx), mechanic(ctx), aesthetic(ctx), affect(ctx), playContext(ctx), session(ctx), utterance(ctx),
              reaction(ctx), designGap(ctx), hypothesis(ctx) {}
        repositories::PersonRepo person;
        repositories::GameRepo game;
        repositories::MechanicRepo mechanic;
        repositories::AestheticRepo aesthetic;
        repositories::AffectRepo affect;
        repositories::PlayContextRepo playContext;
        repositories::SessionRepo session;
        repositories::UtteranceRepo utterance;
        repositories::ReactionRepo reaction;
        repositories::DesignGapRepo designGap;
        repositories::HypothesisRepo hypothesis;
    } repos;

    /** TS の core.client.raw。 */
    sql::Database& raw() { return client.raw(); }
    const sql::DatabasePtr& raw_ptr() const { return client.raw_ptr(); }

    void register_embedding(const vectors::RegisterEmbeddingInput& input) { vectors::register_embedding(raw(), input); }
    std::vector<vectors::SimilarNode> search_similar(const vectors::SearchSimilarInput& input) { return vectors::search_similar(raw(), input); }

    void close() { client.close(); }
};

/** createCore(sqlitePath, eventsPath)。 */
std::unique_ptr<Core> create_core(const CoreOptions& options = {});
std::unique_ptr<Core> create_core(const std::string& sqlite_path, const std::optional<std::string>& events_path = std::nullopt);

} // namespace di::core
