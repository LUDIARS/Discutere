#include "design_diagnostic/documents.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "design_diagnostic/validation.hpp"

namespace di::design_diagnostic {

namespace {

const std::string C = "invalid_input";
const std::vector<std::string> RESERVED = {"spec", "level", "baseline", "playModel"};

DiagnosticReference reference(const Json& value, std::size_t index) {
    const auto& L = DIAGNOSTIC_LIMITS;
    const std::string path = "references[" + std::to_string(index) + "]";
    object(value, path, {"id", "source", "revision", "text", "evidenceKind", "domain"}, C);
    const Json& r = value;
    static const Json kMissing;  // object() 済みなので r[...] が無いときだけ使う
    auto at = [&](const char* k) -> const Json& {
        auto* f = field(r, k);
        return f ? *f : kMissing;
    };
    DiagnosticReference ref;
    ref.id = id(at("id"), path + ".id", C);
    if (std::find(RESERVED.begin(), RESERVED.end(), ref.id) != RESERVED.end()) fail(C, path + ".id", "reserved document ID");
    ref.source = string(at("source"), path + ".source", L.shortTextChars, C);
    if (field(r, "revision")) ref.revision = string(at("revision"), path + ".revision", L.shortTextChars, C);
    ref.text = string(at("text"), path + ".text", L.referenceChars, C);
    ref.evidenceKind = choice(at("evidenceKind"), evidence_kinds(), path + ".evidenceKind", C);
    if (field(r, "domain")) ref.domain = choice(at("domain"), {"mechanics", "level"}, path + ".domain", C);
    return ref;
}

} // namespace

void DocumentMap::set(DiagnosticReference ref) {
    for (auto& item : items_) {
        if (item.id == ref.id) {
            item = std::move(ref);
            return;
        }
    }
    items_.push_back(std::move(ref));
}

const DiagnosticReference* DocumentMap::get(const std::string& id) const {
    for (const auto& item : items_)
        if (item.id == id) return &item;
    return nullptr;
}

std::vector<std::string> DocumentMap::keys() const {
    std::vector<std::string> out;
    for (const auto& item : items_) out.push_back(item.id);
    return out;
}

const Json* field(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

ValidatedDocuments validate_documents(const Json* primary, const Json* baseline, const Json* raw_references, const std::string& primary_field) {
    const auto& L = DIAGNOSTIC_LIMITS;
    static const Json kMissing;
    ValidatedDocuments out;
    out.primaryText = string(primary ? *primary : kMissing, primary_field, L.documentChars, C);
    if (baseline) out.baselineText = string(*baseline, "baselineText", L.documentChars, C);
    if (raw_references) {
        array(*raw_references, "references", L.references, C);
        const Json& arr = *raw_references;
        for (std::size_t i = 0; i < arr.size(); ++i) out.references.push_back(reference(arr[i], i));
    }
    std::vector<std::string> ids;
    for (const auto& r : out.references) ids.push_back(r.id);
    unique(ids, "references", C);
    std::size_t total = str::js_length(out.primaryText) + (out.baselineText ? str::js_length(*out.baselineText) : 0);
    for (const auto& r : out.references) total += str::js_length(r.text);
    if (total > L.totalTextChars) fail(C, "request", "document text exceeds " + std::to_string(L.totalTextChars) + " characters");
    return out;
}

DocumentMap supplied_document_map(const std::string& primary_id, const std::string& text, const std::optional<std::string>& baseline_text,
                                  const std::vector<DiagnosticReference>& references) {
    DocumentMap docs;
    for (const auto& r : references) docs.set(r);
    const std::string domain = primary_id == "spec" ? "mechanics" : "level";
    docs.set(DiagnosticReference{primary_id, "supplied " + primary_id + " text", std::nullopt, text, "specification", domain});
    if (baseline_text)
        docs.set(DiagnosticReference{"baseline", "supplied baselineText", std::nullopt, *baseline_text, "specification", domain});
    return docs;
}

} // namespace di::design_diagnostic
