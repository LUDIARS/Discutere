#include "core/events/event_log.hpp"

#include <stdexcept>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/time.hpp"
#include "core/events/projection.hpp"

namespace di::core::events {

EventLog::EventLog(db::KuzuClient& client, const std::string& jsonl_path, EventReadLimits limits)
    : client_(client), jsonl_path_(fs::resolve(jsonl_path)), limits_(limits) {
    fs::ensure_dir(fs::dirname(jsonl_path_));
    if (!fs::exists(jsonl_path_)) fs::append_file(jsonl_path_, "");
}

DomainEvent EventLog::append_event(const AppendEventInput& event) {
    DomainEvent full;
    full.id = event.id ? *event.id : crypto::uuid_v4();
    full.eventType = event.eventType;
    full.payload = event.payload;
    full.createdAt = event.createdAt ? *event.createdAt : static_cast<double>(time::now_ms());

    const std::string payload_json = json_stringify(full.payload);
    const std::string line = json_stringify(to_json(full)) + "\n";
    // 書く前に大きさを確かめる (DB にも jsonl にも書かない)。
    if (line.size() > kMaxEventLineBytes) throw std::length_error("event exceeds the size limit");

    auto& db = client_.raw();
    db.transaction([&] {
        db.run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES (?, ?, ?, ?)",
               {full.id, full.eventType, payload_json, full.createdAt});
        apply_event_projection(db, full);
    });

    // jsonl は追記だけの写し。全体の上限を超えたら追記をやめる (SoT の events 表は確定済み)。
    std::lock_guard lock(append_mutex_);
    const auto size = fs::file_size(jsonl_path_).value_or(0);
    if (size + line.size() > kMaxEventsJsonlBytes) {
        if (!jsonl_full_warned_) {
            jsonl_full_warned_ = true;
            log::warn("[event-log] events.jsonl reached the size limit; further events are kept only in SQLite");
        }
    } else if (!fs::append_file(jsonl_path_, line)) {
        throw std::runtime_error("failed to append events.jsonl");
    }
    return full;
}

std::vector<DomainEvent> EventLog::rows_to_events(const std::vector<sql::Row>& rows) const {
    std::vector<DomainEvent> out;
    out.reserve(rows.size());
    for (const auto& r : rows) {
        DomainEvent e;
        e.id = r.str("id");
        e.eventType = r.str("event_type");
        std::string error;
        auto payload = parse_json(r.str("payload_json"), limits_.payload, &error);
        // TS は JSON.parse の例外をそのまま投げる (壊れた行は読み出し全体を失敗させる)。
        if (!payload) throw std::runtime_error("events.payload_json: " + error);
        e.payload = std::move(*payload);
        e.createdAt = r.num("created_at");
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<DomainEvent> EventLog::load_events_since(const LoadEventsSinceInput& input) {
    auto& db = client_.raw();
    const sql::QueryLimits q{limits_.max_rows, limits_.max_bytes};
    if (input.eventId && !input.eventId->empty()) {
        return rows_to_events(db.all(
            "SELECT id, event_type, payload_json, created_at FROM events WHERE created_at >= (SELECT created_at FROM events WHERE id = ?) ORDER BY created_at ASC",
            {*input.eventId}, q));
    }
    const double timestamp = input.timestamp.value_or(0);
    return rows_to_events(db.all(
        "SELECT id, event_type, payload_json, created_at FROM events WHERE created_at >= ? ORDER BY created_at ASC",
        {timestamp}, q));
}

void EventLog::rebuild_from_event_store() {
    auto& db = client_.raw();
    const auto events = rows_to_events(db.all("SELECT id, event_type, payload_json, created_at FROM events ORDER BY created_at ASC",
                                              {}, sql::QueryLimits{limits_.max_rows, limits_.max_bytes}));
    db.transaction([&] {
        db.exec("DELETE FROM people; DELETE FROM games; DELETE FROM mechanics; DELETE FROM aesthetics; DELETE FROM affects; DELETE FROM play_contexts; DELETE FROM sessions; DELETE FROM utterances; DELETE FROM reactions; DELETE FROM design_gaps; DELETE FROM hypotheses;");
        for (const auto& e : events) apply_event_projection(db, e);
    });
}

} // namespace di::core::events
