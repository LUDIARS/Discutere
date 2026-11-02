#pragma once
// raw-store — 長文 source の raw 全文を退避する sidecar (id=67) (src/crawler/sources/raw-store.ts)。
//
// 議論コンテキスト参照はトークン節約のため要約 (utterances.raw_content = summary) を
// 使い、raw 全文はここに per-utterance で保管して必要時のみ引く 2 層化。
// utterances 本体の schema は不変 (ingested-store と同じ思想)。
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::crawler::sources {

struct RawDocument {
    std::string utteranceId;
    std::string source;
    std::string url;
    std::string rawText;
    std::int64_t savedAt = 0;
};
Json to_json(const RawDocument& v);

struct RawStoreInput {
    std::string utteranceId;
    std::string source;
    std::string url;
    std::string rawText;
    std::optional<std::int64_t> at;
};

class RawStore {
public:
    virtual ~RawStore() = default;
    /** utteranceId に raw 全文を紐付けて保存 (上書き)。 */
    virtual void set(const RawStoreInput& input) = 0;
    /** utteranceId の raw 全文を引く。無ければ nullopt。 */
    virtual std::optional<RawDocument> get(const std::string& utterance_id) = 0;
    virtual void close() = 0;
};

/** path.resolve("./data/external/.raw.sqlite") */
std::string default_raw_path();

/** 開く (親ディレクトリは作る)。失敗は sql::Error。 */
std::unique_ptr<RawStore> open_raw_store(const std::string& db_path = default_raw_path());

} // namespace di::crawler::sources
