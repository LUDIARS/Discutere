#pragma once
// src/flow/proxy/resolve.ts の移植: 代理議論の起動条件の解決 (dispatch から呼ぶ、S3)。
//
// 観点タグ「代理」が付いた議論/改善だけが対象。起動できない理由はすべて ProxyRequestError で返し、
// 黙って通常の議論に切り替えない (人は代理議論を頼んでいるため)。
//   - flow.proxy.enabled=false
//   - 機密 / 内部 タグ (外部の声と同じく、公開レビュー由来の人物像も使わない)
//   - 議題に「代理: 論者#a, 論者#b」が無い / 人物像が使えない
#include <optional>
#include <string>
#include <vector>

#include "flow/personas.hpp"
#include "flow/tags.hpp"

namespace di::flow::proxy {

struct ProxyRun {
    /** 指定部分を除いた議題。 */
    std::string theme;
    std::vector<FlowPersona> participants;
};

/** 代理議論を求めていなければ nullopt。求めているのに起動できなければ ProxyRequestError。now 省略は現在時刻。 */
std::optional<ProxyRun> resolve_proxy_run(const std::string& theme, const std::vector<FlowTag>& tags,
                                          std::optional<double> now = std::nullopt);

} // namespace di::flow::proxy
