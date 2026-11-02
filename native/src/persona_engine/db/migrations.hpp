#pragma once
// persona-engine DB migration (src/persona-engine/db/migrations.ts, self-contained)。
// apply_persona_engine_migrations(db) を呼ぶだけで必要 table が揃う。`_pe_migrations` を独自に持つ。
// DDL は TS と同じ文 (既存の DB ファイルをそのまま読める)。
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::persona_engine::db {

struct Migration {
    std::string id;
    std::vector<std::string> sql;
};

/** 適用する migration の一覧 (TS の MIGRATIONS と同じ順・同じ文)。 */
const std::vector<Migration>& persona_engine_migrations();

void apply_persona_engine_migrations(sql::Database& db);

} // namespace di::persona_engine::db
