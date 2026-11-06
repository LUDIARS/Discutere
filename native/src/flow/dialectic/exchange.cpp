#include "flow/dialectic/exchange.hpp"

#include <stdexcept>

namespace di::flow::dialectic {

ArgumentNode GroundExchange::add_opening(const NewNode& node) { return insert(node, std::nullopt, 0); }

std::optional<ArgumentNode> GroundExchange::next() {
    while (queue_head_ < queue_.size()) {
        auto it = index_.find(queue_[queue_head_]);
        if (it != index_.end() && nodes_[it->second].status == "open") return nodes_[it->second];
        ++queue_head_;
    }
    return std::nullopt;
}

ArgumentNode GroundExchange::rebut(const std::string& target_id, const NewNode& node) {
    auto& target = require(target_id);
    close(target, "answered");
    const std::string parent = target.id;
    const int depth = target.depth + 1;
    return insert(node, parent, depth);
}

void GroundExchange::accept(const std::string& target_id) { close(require(target_id), "accepted"); }

void GroundExchange::fail(const std::string& target_id) { close(require(target_id), "answered"); }

int GroundExchange::cut_remaining() {
    int n = 0;
    for (std::size_t i = queue_head_; i < queue_.size(); ++i) {
        auto it = index_.find(queue_[i]);
        if (it != index_.end() && nodes_[it->second].status == "open") {
            nodes_[it->second].status = "cut";
            ++n;
        }
    }
    queue_.clear();
    queue_head_ = 0;
    return n;
}

bool GroundExchange::settled() { return !next().has_value(); }

std::optional<ArgumentNode> GroundExchange::get(const std::string& id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return nodes_[it->second];
}

std::vector<ArgumentNode> GroundExchange::all() const { return nodes_; }

std::vector<ArgumentNode> GroundExchange::children(const std::string& id) const {
    std::vector<ArgumentNode> out;
    for (const auto& n : nodes_)
        if (n.parentId && *n.parentId == id) out.push_back(n);
    return out;
}

std::vector<ArgumentNode> GroundExchange::path(const std::string& id) const {
    std::vector<ArgumentNode> out;
    auto cur = get(id);
    // 親を辿る (ノード数を超えたら打ち切る = 循環の防御)。
    while (cur && out.size() <= nodes_.size()) {
        out.insert(out.begin(), *cur);
        cur = cur->parentId ? get(*cur->parentId) : std::nullopt;
    }
    return out;
}

NodeVerdict GroundExchange::verdict(const std::string& id) const {
    const auto& node = require(id);
    if (node.status == "cut") return "contested";
    bool contested = false;
    for (const auto& child : children(id)) {
        const auto v = verdict(child.id);
        if (v == "standing") return "refuted";
        if (v == "contested") contested = true;
    }
    return contested ? "contested" : "standing";
}

std::vector<std::pair<std::string, GroundState>> GroundExchange::ground_states() const {
    std::vector<std::pair<std::string, GroundState>> out;
    for (const auto& node : nodes_) {
        if (node.depth != 0 || !node.groundId) continue;
        const auto kids = children(node.id);
        GroundState state;
        if (kids.empty()) state = "unchallenged";
        else if (verdict(node.id) != "refuted") state = "defended";
        else {
            bool accepted = false;
            for (const auto& k : kids)
                if (k.status == "accepted") accepted = true;
            state = accepted ? "conceded" : "challenged";
        }
        // Map.set と同じく同じ groundId は値だけ上書き (位置は最初のまま)。
        bool replaced = false;
        for (auto& [gid, s] : out)
            if (gid == *node.groundId) {
                s = state;
                replaced = true;
            }
        if (!replaced) out.emplace_back(*node.groundId, state);
    }
    return out;
}

GroundExchange::Count GroundExchange::standing_count(const Side& side) const {
    Count c;
    for (const auto& node : nodes_) {
        if (node.depth != 0 || node.side != side) continue;
        ++c.total;
        if (verdict(node.id) != "refuted") ++c.standing;
    }
    return c;
}

ArgumentNode GroundExchange::insert(const NewNode& node, std::optional<std::string> parent_id, int depth) {
    ArgumentNode full;
    full.id = "N" + std::to_string(++seq_);
    full.utteranceId = node.utteranceId;
    full.side = node.side;
    full.personaId = node.personaId;
    full.parentId = std::move(parent_id);
    full.depth = depth;
    full.groundId = node.groundId;
    full.data = node.data;
    full.warrant = node.warrant;
    full.kind = node.kind;
    full.text = node.text;
    full.status = depth >= options_.maxDepth ? "cut" : "open";
    index_[full.id] = nodes_.size();
    nodes_.push_back(full);
    if (full.status == "open") queue_.push_back(full.id);
    return full;
}

void GroundExchange::close(ArgumentNode& node, const NodeStatus& status) {
    if (node.status == "open") node.status = status;
}

ArgumentNode& GroundExchange::require(const std::string& id) {
    auto it = index_.find(id);
    if (it == index_.end()) throw std::invalid_argument("unknown argument node: " + id);
    return nodes_[it->second];
}

const ArgumentNode& GroundExchange::require(const std::string& id) const {
    auto it = index_.find(id);
    if (it == index_.end()) throw std::invalid_argument("unknown argument node: " + id);
    return nodes_[it->second];
}

Side opposing_side(const Side& side) { return side == "pro" ? "con" : "pro"; }

} // namespace di::flow::dialectic
