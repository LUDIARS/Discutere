#pragma once
// ドメインイベントを SQLite の索引表へ射影する (src/core/events/projection.ts)。
// payload の値は better-sqlite3 と同じ規則で bind する (core/db/js_bind.hpp)。
#include "base/sqlite.hpp"
#include "core/events/event_types.hpp"

namespace di::core::events {

void apply_event_projection(sql::Database& db, const DomainEvent& event);

} // namespace di::core::events
