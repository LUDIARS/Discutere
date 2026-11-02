#pragma once
// src/flow/dialectic/exchange.ts の移植: 論拠の応酬 [2] (dialectic.md §2.6) — 決定的コード・LLM 不使用。
// 論拠 1 件 = 1 ノード。反論も論拠 1 件のノードとして親にぶら下げ、幅優先のキューで相手陣営が答える。
// 決着は三値 (standing / refuted / contested)。打ち切りは contested とし根拠を崩さない。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/premise_rules.hpp"
#include "flow/dialectic/store.hpp"

namespace di::flow::dialectic {

/** "pro" | "con" */
using Side = std::string;
/** "open" | "answered" | "accepted" | "cut" */
using NodeStatus = std::string;
/** "standing" | "refuted" | "contested" */
using NodeVerdict = std::string;

struct ArgumentNode {
    std::string id;
    std::string utteranceId;
    Side side;
    std::string personaId;
    std::optional<std::string> parentId;
    int depth = 0;
    /** 定立の論拠なら Position の根拠 id (pro-G1 等)。反論は nullopt。 */
    std::optional<std::string> groundId;
    std::string data;
    std::optional<std::string> warrant;
    std::optional<InferenceKind> kind;
    std::string text;
    NodeStatus status = "open";
};

/** Omit<ArgumentNode, "id" | "depth" | "status" | "parentId"> */
struct NewNode {
    std::string utteranceId;
    Side side;
    std::string personaId;
    std::optional<std::string> groundId;
    std::string data;
    std::optional<std::string> warrant;
    std::optional<InferenceKind> kind;
    std::string text;
};

struct ExchangeOptions {
    /** 反論の深さ上限 (定立の論拠 = 0)。この深さのノードには答えさせない。 */
    int maxDepth = 0;
};

class GroundExchange {
public:
    explicit GroundExchange(ExchangeOptions options) : options_(options) {}
    ArgumentNode add_opening(const NewNode& node);
    /** 次に答えるべきノード (幅優先)。 */
    std::optional<ArgumentNode> next();
    /** target への反論を登録する (未知の target は std::invalid_argument)。 */
    ArgumentNode rebut(const std::string& target_id, const NewNode& node);
    void accept(const std::string& target_id);
    void fail(const std::string& target_id);
    /** 残りの応答待ちをすべて打ち切る。打ち切った件数を返す。 */
    int cut_remaining();
    bool settled();
    std::optional<ArgumentNode> get(const std::string& id) const;
    /** 挿入順。 */
    std::vector<ArgumentNode> all() const;
    std::vector<ArgumentNode> children(const std::string& id) const;
    /** 定立の論拠から node までの経路 (根が先頭)。 */
    std::vector<ArgumentNode> path(const std::string& id) const;
    NodeVerdict verdict(const std::string& id) const;
    /** 定立の論拠の決着を Position の根拠状態へ写す (groundId → state、挿入順)。 */
    std::vector<std::pair<std::string, GroundState>> ground_states() const;
    struct Count {
        int standing = 0;
        int total = 0;
    };
    Count standing_count(const Side& side) const;

private:
    ArgumentNode insert(const NewNode& node, std::optional<std::string> parent_id, int depth);
    void close(ArgumentNode& node, const NodeStatus& status);
    ArgumentNode& require(const std::string& id);
    const ArgumentNode& require(const std::string& id) const;

    ExchangeOptions options_;
    std::vector<ArgumentNode> nodes_;
    std::map<std::string, std::size_t> index_;
    std::vector<std::string> queue_;
    std::size_t queue_head_ = 0;
    int seq_ = 0;
};

Side opposing_side(const Side& side);

} // namespace di::flow::dialectic
