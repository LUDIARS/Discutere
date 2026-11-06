#pragma once
// External importer (Phase 1) — spec/feature/crawler/EXTERNAL-SOURCES.md §3 (src/crawler/sources/importer.ts の移植)。
//
// ExternalUtterance[] を Discatier Core の sessions / utterances / reactions に
// 格納する。決定的 id (`ext:<source>:<nativeId>`) と dedup sidecar で冪等。
//
// - session_id   = `ext:<source>:<threadKey>` (議論の場 = 1 session)
// - utterance.id = `ext:<source>:<nativeId>`  (再 import で重複 INSERT しないよう存在確認)
// - speaker_id   = `ext:<source>:<authorId>`  (公開 ID アンカー、 露出時に persona へマスク)
// - responds_to  = `ext:<source>:<parentNativeId>`
// - signal.votedUp → reactions (positive/negative, intensity=upvotes)
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/ingested_store.hpp"
#include "crawler/sources/raw_store.hpp"
#include "crawler/sources/types.hpp"

namespace di::crawler::sources {

struct ExternalImportResult {
    double sessions = 0;
    double utterances = 0;
    double reactions = 0;
    double skipped = 0;
    /** summary を本文に採用し raw を raw-store へ退避した件数 (2 層化、 id=67) */
    double summarized = 0;
};
Json to_json(const ExternalImportResult& v);

struct ExternalImportOptions {
    std::string workspaceId;
    /** dedup sidecar。渡せば取り込み済み (source,nativeId) を skip + 記録する */
    IngestedStore* ingested = nullptr;
    /**
     * raw 全文の退避先 (id=67)。 item.summary がある時、 utterance 本文は summary に、
     * raw 全文 (content) はこの store に保存する (トークン節約 2 層化)。 未指定なら raw を本文に。
     */
    RawStore* rawStore = nullptr;
    /** 出所メタ sidecar (§3 / §6)。 渡せば utterance 採番直後に source / sourceUrl / nativeId / gameSlug を upsert する。 */
    AttributionStore* attribution = nullptr;
    /** session タイトル生成 (既定 `<source>:<gameSlug>`) */
    std::function<std::string(const ExternalUtterance&)> sessionTitle;
};

/** 1 回の取込で受ける件数の上限 (移植で追加。超えた分は skipped に数える)。 */
inline constexpr std::size_t kImportMaxItems = 100'000;

/** `ext:<source>:<id>`。 */
std::string ext_id(const std::string& source, const std::string& native_id);

ExternalImportResult import_external_utterances(core::Core& core, const std::vector<ExternalUtterance>& items,
                                                const ExternalImportOptions& opts);

} // namespace di::crawler::sources
