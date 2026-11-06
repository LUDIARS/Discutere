#pragma once
// dedup sidecar — spec/feature/crawler/EXTERNAL-SOURCES.md §3 dedup (src/crawler/sources/ingested-store.ts)。
//
// 取り込み済み (source, nativeId) を記録する小さな SQLite。utterances 本体には
// 外部 id 列を足さない (schema 不変) ため、冪等性はこの sidecar が担保する。
#include <memory>
#include <optional>
#include <string>

#include "base/sqlite.hpp"
#include "crawler/sources/types.hpp"

namespace di::crawler::sources {

class IngestedStore {
public:
    virtual ~IngestedStore() = default;
    virtual bool has(const ExternalSource& source, const std::string& native_id) = 0;
    /** at 省略は現在時刻 (ms)。 */
    virtual void add(const ExternalSource& source, const std::string& native_id, std::optional<std::int64_t> at = std::nullopt) = 0;
    virtual void close() = 0;
};

/** path.resolve("./data/external/.ingested.sqlite") */
std::string default_ingested_path();

/** 開く (親ディレクトリは作る)。失敗は sql::Error。 */
std::unique_ptr<IngestedStore> open_ingested_store(const std::string& db_path = default_ingested_path());

} // namespace di::crawler::sources
