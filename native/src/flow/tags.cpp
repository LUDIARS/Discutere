#include "flow/tags.hpp"

#include <algorithm>

namespace di::flow {

namespace {
bool has(const std::vector<FlowTag>& tags, std::string_view tag) {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}
} // namespace

bool wants_foresight(const std::vector<FlowTag>& tags) { return has(tags, FORESIGHT_TAG); }
bool wants_proxy(const std::vector<FlowTag>& tags) { return has(tags, PROXY_TAG); }

bool can_collect_external(const std::vector<FlowTag>& tags) {
    // 収集禁止タグ群
    return !(has(tags, "機密") || has(tags, "内部"));
}

std::string paper_supplement(const std::vector<FlowTag>& tags) {
    std::vector<std::string> lines;
    if (has(tags, "開発")) lines.emplace_back("【開発者観点】設計意図・実装コスト・技術的トレードオフを踏まえて議論してください。");
    if (has(tags, "運用")) lines.emplace_back("【運用者観点】継続率・収益・サーバ負荷・運用コストを踏まえて議論してください。");
    if (has(tags, "内部")) lines.emplace_back("【内部情報】外部には非公開の社内情報・未発表の情報を前提に議論してください。");
    if (has(tags, "機密"))
        lines.emplace_back("【機密】実ユーザデータが使用できないため、メカニクス評価から合成した想定意見データを参考にしています。");
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n";
        out += lines[i];
    }
    return out;
}

} // namespace di::flow
