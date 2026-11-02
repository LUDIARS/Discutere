#pragma once
// src/flow/persona-import-document.ts の移植 (@spec インポート Vo → Di)。
// JSON 1 つ (配列 / {personas: [...]} / 単体) か、壊れていれば NDJSON (1 行 1 件) として読む。
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::flow {

struct PersonaDocument {
    std::vector<Json> personas;
    int invalidJsonLines = 0;
};

/** 文書全体 / 1 行あたりの JSON の上限 (外部入力)。 */
JsonLimits persona_document_json_limits();

PersonaDocument parse_persona_document(std::string_view text);

} // namespace di::flow
