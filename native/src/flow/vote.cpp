#include "flow/vote.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "flow/cost_logger.hpp"
#include "flow/db/connection.hpp"
#include "flow/turn_scheduler.hpp"

namespace di::flow {

Json to_json(const VoteResult& r) {
    Json o = Json::object();
    if (r.kind) o["kind"] = *r.kind;
    Json tally = Json::object();
    for (const auto& [id, n] : r.tally) tally[id] = n;
    o["tally"] = std::move(tally);
    o["winner"] = r.winner ? Json(*r.winner) : Json(nullptr);
    o["winnerShare"] = r.winnerShare ? Json(*r.winnerShare) : Json(nullptr);
    return o;
}

std::string build_vote_prompt(const std::string& theme, const std::vector<RoundUtterance>& utterances, const VoteLens& lens) {
    std::vector<std::string> list;
    for (std::size_t i = 0; i < utterances.size(); ++i)
        list.push_back(std::to_string(i + 1) + ". [ID:" + utterances[i].id + "] " + utterances[i].text);
    return "議論のテーマ: " + theme + "\n\n" + "このラウンドの発言一覧:\n" + str::join(list, "\n") + "\n\n" +
           "あなたは中立な審判として、" + lens.instruction + "。" + "テーマに対して最も的を射た意見を 1 つ選び、" +
           " その発言の ID (または先頭の番号) だけを返してください。\n" + "返答形式 (IDのみ): <選んだID>\n" +
           "例: abc-123\n" + "返答はIDまたは番号のみ (説明不要)。";
}

std::optional<std::string> parse_vote_id(const std::string& text, const std::vector<std::string>& valid_ids,
                                         const std::vector<std::string>& presented) {
    const std::string trimmed = str::trim(text);
    // 完全一致
    if (std::find(valid_ids.begin(), valid_ids.end(), trimmed) != valid_ids.end()) return trimmed;
    // 部分一致 (LLM が余分なテキストを含む場合)。Set の挿入順に見る。
    for (const auto& id : valid_ids)
        if (str::contains(trimmed, id)) return id;
    // 番号回答: /^(\d{1,3})\s*(?:[.。]|番)?\s*$/
    std::size_t i = 0;
    while (i < trimmed.size() && trimmed[i] >= '0' && trimmed[i] <= '9') ++i;
    if (i == 0 || i > 3) return std::nullopt;
    const int idx = std::stoi(trimmed.substr(0, i)) - 1;
    auto skip_ws = [&](std::size_t p) {
        while (p < trimmed.size()) {
            std::size_t next = p;
            if (!str::is_js_space(str::next_codepoint(trimmed, next))) break;
            p = next;
        }
        return p;
    };
    std::size_t p = skip_ws(i);
    if (trimmed.compare(p, 1, ".") == 0) p += 1;
    else if (trimmed.compare(p, 3, "。") == 0 || trimmed.compare(p, 3, "番") == 0) p += 3;
    p = skip_ws(p);
    if (p != trimmed.size()) return std::nullopt;
    if (idx >= 0 && static_cast<std::size_t>(idx) < presented.size()) return presented[static_cast<std::size_t>(idx)];
    return std::nullopt;
}

namespace {

/** JS の既定 sort と同じ UTF-16 コード単位順。 */
bool utf16_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    // 1 コードポイントを UTF-16 のコード単位 (1 か 2 個) にする。
    auto units = [](std::uint32_t cp, std::array<char16_t, 2>& out) -> std::size_t {
        if (cp >= 0x10000) {
            out[0] = static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10));
            out[1] = static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
            return 2;
        }
        out[0] = static_cast<char16_t>(cp);
        return 1;
    };
    std::array<char16_t, 2> ua{}, ub{};
    std::size_t na = 0, nb = 0, ia = 0, ib = 0;
    while (true) {
        if (ia == na) {
            if (i >= a.size()) return !(j >= b.size() && ib == nb);
            na = units(str::next_codepoint(a, i), ua);
            ia = 0;
        }
        if (ib == nb) {
            if (j >= b.size()) return false;
            nb = units(str::next_codepoint(b, j), ub);
            ib = 0;
        }
        // units は 1 か 2 を返すので ia < na <= 2 / ib < nb <= 2。範囲は明示して確かめる。
        if (ia >= ua.size() || ib >= ub.size()) return false;
        if (ua[ia] != ub[ib]) return ua[ia] < ub[ib];
        ++ia;
        ++ib;
    }
}

} // namespace

VoteWinner pick_vote_winner(const std::vector<std::pair<std::string, double>>& tally, const Rng& rng) {
    double max_votes = 0;
    for (const auto& [id, v] : tally)
        if (v > max_votes) max_votes = v;
    if (max_votes <= 0) return {std::nullopt, 0};
    std::vector<std::string> top;
    for (const auto& [id, v] : tally)
        if (v == max_votes) top.push_back(id);
    std::sort(top.begin(), top.end(), utf16_less);
    if (top.size() == 1) return {top[0], max_votes};
    double f = std::floor(rng() * static_cast<double>(top.size()));
    // rng が [0,1) を外れても範囲外を引かない (移植前は undefined)。
    if (!(f >= 0)) f = 0;
    if (f > static_cast<double>(top.size() - 1)) f = static_cast<double>(top.size() - 1);
    return {top[static_cast<std::size_t>(f)], max_votes};
}

VoteResult run_round_vote(persona_engine::LLMClient& llm, const RunRoundVoteArgs& args) {
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string& m) { log::warn(m); });
    const Rng rng = args.rng ? args.rng : Rng([] { return crypto::random_unit(); });
    const auto& lenses = args.lenses ? *args.lenses : DEFAULT_VOTE_LENSES();

    if (args.utterances.empty()) return {std::string("votes"), {}, std::nullopt, std::nullopt};

    std::vector<std::string> valid_ids;
    for (const auto& u : args.utterances)
        if (std::find(valid_ids.begin(), valid_ids.end(), u.id) == valid_ids.end()) valid_ids.push_back(u.id);
    std::vector<std::pair<std::string, double>> tally;
    int valid_votes = 0;
    auto db = db::get_flow_db();
    const std::string round = std::to_string(args.round);

    for (int voter = 0; voter < args.voterCount; ++voter) {
        // レンズは巡回割当、候補提示順は投票者ごとにシャッフル (位置バイアスの脱相関)。
        const VoteLens& lens = lenses.empty() ? DEFAULT_VOTE_LENSES()[0] : lenses[static_cast<std::size_t>(voter) % lenses.size()];
        const auto presented = shuffle(args.utterances, rng);
        auto logged = with_cost_log(llm, {"discussion", args.sessionId, static_cast<double>(args.round), {}, {}, {}, "vote", {}});
        persona_engine::LLMInvokeArgs call;
        call.prompt = build_vote_prompt(args.theme, presented, lens);
        const auto result = logged.invoke(call);

        std::optional<std::string> chosen;
        if (!result.ok) {
            warn("投票 round=" + round + " voter=" + std::to_string(voter) + " (" + lens.key + ") 失敗: " + result.error);
        } else {
            std::vector<std::string> presented_ids;
            for (const auto& u : presented) presented_ids.push_back(u.id);
            chosen = parse_vote_id(result.text, valid_ids, presented_ids);
            if (!chosen) {
                warn("投票 round=" + round + " voter=" + std::to_string(voter) + " (" + lens.key +
                     "): 有効な ID を解析できませんでした (" + str::js_slice(result.text, 0, 60) + ")");
            } else {
                auto it = std::find_if(tally.begin(), tally.end(), [&](const auto& kv) { return kv.first == *chosen; });
                if (it == tally.end()) tally.emplace_back(*chosen, 1);
                else it->second += 1;
                ++valid_votes;
            }
        }
        db->run("INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?, ?, ?, ?, ?)",
                {args.sessionId, args.round, voter, sql::Value(chosen), time::now_ms()});
    }

    const auto [winner, max_votes] = pick_vote_winner(tally, rng);
    VoteResult out;
    out.kind = "votes";
    out.tally = std::move(tally);
    out.winner = winner;
    if (valid_votes > 0 && winner) out.winnerShare = max_votes / valid_votes;
    return out;
}

} // namespace di::flow
