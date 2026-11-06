#include "level_check/request.hpp"

#include <set>

#include "design_diagnostic/validation.hpp"
#include "level_check/play_model_validation.hpp"

namespace di::level_check {

namespace dd = design_diagnostic;

LevelCheckRequest validate_level_check_request(const Json& value) {
    dd::object(value, "request", {"levelText", "baselineText", "references", "playModel"}, "invalid_input");
    auto docs_in = dd::validate_documents(dd::field(value, "levelText"), dd::field(value, "baselineText"), dd::field(value, "references"), "levelText");
    const auto docs = dd::supplied_document_map("level", docs_in.primaryText, docs_in.baselineText, docs_in.references);
    LevelCheckRequest r;
    r.levelText = std::move(docs_in.primaryText);
    r.baselineText = std::move(docs_in.baselineText);
    r.references = std::move(docs_in.references);
    if (const Json* model = dd::field(value, "playModel")) {
        const auto keys = docs.keys();
        r.playModel = validate_play_model(*model, std::set<std::string>(keys.begin(), keys.end()));
    }
    return r;
}

dd::DocumentMap supplied_level_documents(const LevelCheckRequest& request) {
    auto docs = dd::supplied_document_map("level", request.levelText, request.baselineText,
                                          request.references.value_or(std::vector<LevelReference>{}));
    if (request.playModel)
        docs.set(LevelReference{"playModel", "supplied conditional model (not observed play)", std::nullopt, json_stringify(to_json(*request.playModel)),
                                "model_assumption", std::nullopt});
    return docs;
}

} // namespace di::level_check
