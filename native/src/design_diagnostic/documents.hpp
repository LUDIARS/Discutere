#pragma once
// Validate supplied text and provenance; never fetch a referenced source (src/design-diagnostic/documents.ts)。
// JSON の「キーが無い」(TS の undefined) は nullptr の Json* で表す。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "design_diagnostic/contracts.hpp"

namespace di::design_diagnostic {

/** 挿入順を保つ id → DiagnosticReference (TS の Map。同じ id の set はその位置で置き換える)。 */
class DocumentMap {
public:
    void set(DiagnosticReference ref);
    const DiagnosticReference* get(const std::string& id) const;
    bool has(const std::string& id) const { return get(id) != nullptr; }
    const std::vector<DiagnosticReference>& values() const { return items_; }
    std::vector<std::string> keys() const;

private:
    std::vector<DiagnosticReference> items_;
};

struct ValidatedDocuments {
    std::string primaryText;
    std::optional<std::string> baselineText;
    std::vector<DiagnosticReference> references;
};

/** object(value) から key を引く (無ければ nullptr = undefined)。 */
const Json* field(const Json& object, const char* key);

/**
 * primary / baseline / references を検証する (失敗は DiagnosticError("invalid_input", ...))。
 * primary_field は "specText" | "levelText"。
 */
ValidatedDocuments validate_documents(const Json* primary, const Json* baseline, const Json* raw_references,
                                      const std::string& primary_field);

/** 主文書 (primary_id = "spec" | "level") と baseline を references に足した文書表。 */
DocumentMap supplied_document_map(const std::string& primary_id, const std::string& text, const std::optional<std::string>& baseline_text,
                                  const std::vector<DiagnosticReference>& references);

} // namespace di::design_diagnostic
