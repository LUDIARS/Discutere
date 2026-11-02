#pragma once
// src/flow/proxy/cast.ts の移植: 代理議論のキャスト (統合レビュー §2.2、S3)。
//
// - 議題文から「代理: 論者#aaaaaa, 論者#bbbbbb」(と任意の「ジャンル: xxx」) を読み取る (正規表現は線形の走査に写す)
// - 取り込み済みの人物パックから 2 人を引き、キャストに出してよいか (失効・期限・材料・評価) を確かめる
// - 束を作って FlowPersona (debater、立場は論点ごとに driver が決める) にする
// 欠員を架空の人物で埋めない。足りなければ理由付きで ProxyRequestError を投げる。
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/personas.hpp"
#include "flow/proxy/store.hpp"

namespace di::flow::proxy {

/** 代理議論で露出名の括弧内に出す区分。 */
inline constexpr const char* PROXY_POSSESSION_LABEL = "AI代理";

class ProxyRequestError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ProxyRequest {
    std::vector<std::string> labels;
    std::optional<std::string> genre;
    /** 指定部分を取り除いた議題。 */
    std::string theme;
};

Json to_json(const ProxyRequest& r);

/** 議題文から代理の指定を読み取る。指定が無ければ nullopt。 */
std::optional<ProxyRequest> parse_proxy_request(const std::string& text);

/** 2 人に共通するジャンルを 1 つに決める (指定があればそれ)。決められなければ ProxyRequestError。 */
std::string choose_genre(const std::vector<std::vector<ProxyProfileRow>>& rows_by_label, const std::optional<std::string>& requested);

struct LoadProxyParticipantsArgs {
    ProxyRequest request;
    double now = 0;
    bool requirePassed = true;
    double budgetTokens = 0;
    std::vector<std::string> models;
    bool isLocal = false;
    std::vector<std::string> focusTerms;
};

/** 代理 2 人を FlowPersona にする。 */
std::vector<FlowPersona> load_proxy_participants(const LoadProxyParticipantsArgs& args);

} // namespace di::flow::proxy
