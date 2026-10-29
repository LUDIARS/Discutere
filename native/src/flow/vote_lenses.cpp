#include "flow/vote_lenses.hpp"

namespace di::flow {

const std::vector<VoteLens>& DEFAULT_VOTE_LENSES() {
    static const std::vector<VoteLens> lenses = {
        {"logic", "論理の一貫性・飛躍のなさを最重視して選ぶ"},
        {"grounds", "根拠・具体例の裏付けを最重視して選ぶ"},
        {"relevance", "テーマへの適合・議論を前に進める度合いを最重視して選ぶ"},
    };
    return lenses;
}

Json to_json(const VoteLens& lens) {
    Json out = Json::object();
    out["key"] = lens.key;
    out["instruction"] = lens.instruction;
    return out;
}

VoteLens vote_lens_from_json(const Json& value) {
    if (!value.is_object()) return {};
    return {json_string(value, "key"), json_string(value, "instruction")};
}

} // namespace di::flow
