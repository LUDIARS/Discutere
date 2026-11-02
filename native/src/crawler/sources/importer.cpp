#include "crawler/sources/importer.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "base/strings.hpp"
#include "crawler/sources/persona.hpp"

namespace di::crawler::sources {

Json to_json(const ExternalImportResult& v) {
    return Json{{"sessions", v.sessions}, {"utterances", v.utterances}, {"reactions", v.reactions}, {"skipped", v.skipped},
                {"summarized", v.summarized}};
}

std::string ext_id(const std::string& source, const std::string& native_id) { return "ext:" + source + ":" + native_id; }

namespace {

/** session 行が無ければ作る。新規作成したら true (TS と同じく create は毎回呼ぶ)。 */
bool ensure_session(core::Core& core, const std::string& ws, const std::string& session_id, const ExternalUtterance& item,
                    const ExternalImportOptions& opts) {
    const bool exists = core.raw().get("SELECT 1 FROM sessions WHERE id = ?", {session_id}).has_value();
    const std::string title = opts.sessionTitle ? opts.sessionTitle(item) : item.source + ":" + item.gameSlug;
    core::repositories::SessionInput in;
    in.id = session_id;
    in.workspaceId = ws;
    in.title = title;
    in.startedAt = item.postedAt;
    in.mode = "external";
    core.repos.session.create(in);
    return !exists;
}

bool utterance_exists(core::Core& core, const std::string& id) {
    return core.raw().get("SELECT 1 FROM utterances WHERE id = ?", {id}).has_value();
}

} // namespace

ExternalImportResult import_external_utterances(core::Core& core, const std::vector<ExternalUtterance>& items,
                                                const ExternalImportOptions& opts) {
    const std::string& ws = opts.workspaceId;
    ExternalImportResult result;
    std::set<std::string> ensured_sessions;

    // 親→子 (postedAt 昇順) に並べ、responds_to 先が先に入るようにする。
    // 安定ソート。postedAt が NaN の項目は末尾に回す (JS の sort は NaN の比較が不定)。
    std::vector<const ExternalUtterance*> ordered;
    const std::size_t take = std::min(items.size(), kImportMaxItems);
    ordered.reserve(take);
    for (std::size_t i = 0; i < take; ++i) ordered.push_back(&items[i]);
    result.skipped += static_cast<double>(items.size() - take);
    std::stable_sort(ordered.begin(), ordered.end(), [](const ExternalUtterance* a, const ExternalUtterance* b) {
        const bool an = std::isnan(a->postedAt), bn = std::isnan(b->postedAt);
        if (an || bn) return !an && bn;
        return a->postedAt < b->postedAt;
    });

    for (const ExternalUtterance* p : ordered) {
        const ExternalUtterance& item = *p;
        if (opts.ingested && opts.ingested->has(item.source, item.nativeId)) {
            result.skipped += 1;
            continue;
        }

        const std::string session_id = ext_id(item.source, item.threadKey);
        if (!ensured_sessions.count(session_id)) {
            if (ensure_session(core, ws, session_id, item, opts)) result.sessions += 1;
            ensured_sessions.insert(session_id);
        }

        const std::string utterance_id = ext_id(item.source, item.nativeId);
        if (utterance_exists(core, utterance_id)) {
            // 既に DB にある (sidecar 未使用での再実行等) → 記録だけ更新して skip
            if (opts.ingested) opts.ingested->add(item.source, item.nativeId);
            result.skipped += 1;
            continue;
        }

        const std::string speaker_id = to_speaker_id(item.source, item.authorId);
        std::optional<std::string> responds_to;
        if (item.parentNativeId && !item.parentNativeId->empty()) responds_to = ext_id(item.source, *item.parentNativeId);
        // 2 層化: summary があれば本文は要約 (参照される側 = トークン節約)、 raw 全文は退避。
        const bool use_summary = item.summary && !str::trim(*item.summary).empty() && opts.rawStore;
        const std::string body_content = use_summary ? str::trim(*item.summary) : item.content;
        core::repositories::UtteranceInput u;
        u.id = utterance_id;
        u.workspaceId = ws;
        u.sessionId = session_id;
        u.speakerId = speaker_id;
        u.rawContent = body_content;
        u.postedAt = item.postedAt;
        u.respondsTo = responds_to;
        core.repos.utterance.create(u);
        if (use_summary) {
            opts.rawStore->set(RawStoreInput{utterance_id, item.source, item.sourceUrl, item.content, std::nullopt});
            result.summarized += 1;
        }
        // 出所メタを保持 (全 source)。 露出層が個人をマスクしつつ source/sourceUrl を開示する素 (§6)。
        if (opts.attribution) {
            opts.attribution->set(AttributionInput{utterance_id, item.source, item.sourceUrl, item.nativeId, item.gameSlug, std::nullopt});
        }
        result.utterances += 1;

        if (item.signal && item.signal->votedUp.has_value()) {
            core::repositories::ReactionInput r;
            r.id = utterance_id + ":vote";
            r.workspaceId = ws;
            r.utteranceId = utterance_id;
            r.actorId = speaker_id;
            r.reactionType = *item.signal->votedUp ? "positive" : "negative";
            r.intensity = item.signal->upvotes;
            core.repos.reaction.create(r);
            result.reactions += 1;
        }

        if (opts.ingested) opts.ingested->add(item.source, item.nativeId);
    }

    return result;
}

} // namespace di::crawler::sources
