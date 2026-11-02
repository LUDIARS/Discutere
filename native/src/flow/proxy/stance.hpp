#pragma once
// src/flow/proxy/stance.ts の移植: 代理の立場判定 (統合レビュー §2.2「代理キャスト」)。
//
// 代理は「本人ならどちらか」を本人の発言から決める。論点ごとに、束だけを材料に 賛成 / 反対 / どちらでもない を
// 判定 LLM に答えさせる。障害・ラベル不明は「どちらでもない」に倒す。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::proxy {

/** "pro" | "con" | "neutral" */
using ProxyStance = std::string;

std::string build_proxy_stance_prompt(const std::string& issue_title, const std::string& bundle_text);

/** 単語境界つきで最初に現れたラベル (neutral / pro / con)。無ければ nullopt。 */
std::optional<ProxyStance> parse_proxy_stance(const std::string& text);

struct JudgeProxyStanceArgs {
    std::string issueTitle;
    std::string bundleText;
    /** withCostLog 済み判定 LLM (location="proxy-stance")。 */
    persona_engine::LLMClient* llm = nullptr;
    std::optional<std::string> model;
    std::function<void(const std::string&)> warn;
};

ProxyStance judge_proxy_stance(const JudgeProxyStanceArgs& args);

/** { opposed: true, pro, con } | { opposed: false, reason } */
struct IssueSides {
    bool opposed = false;
    int pro = -1;
    int con = -1;
    std::string reason;
};

Json to_json(const IssueSides& s);

/** 賛成と反対に分かれたときだけ対立にする。 */
IssueSides assign_issue_sides(const std::vector<ProxyStance>& stances, const std::vector<std::string>& labels);

} // namespace di::flow::proxy
