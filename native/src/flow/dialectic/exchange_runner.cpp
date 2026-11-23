#include "flow/dialectic/exchange_runner.hpp"

#include <limits>
#include <stdexcept>

#include "flow/dialectic/drilldown.hpp"
#include "flow/dialectic/exchange_prompt.hpp"
#include "flow/dialectic/rule_check.hpp"
#include "flow/dialectic/scheduler.hpp"

namespace di::flow::dialectic {

void PositionsBySide::set(const Side& side, PositionRecord* position) {
    for (auto& [s, p] : entries_)
        if (s == side) {
            p = position;
            return;
        }
    entries_.emplace_back(side, position);
}

PositionRecord* PositionsBySide::get(const Side& side) const {
    for (const auto& [s, p] : entries_)
        if (s == side) return p;
    return nullptr;
}

std::vector<PositionRecord*> PositionsBySide::values() const {
    std::vector<PositionRecord*> out;
    for (const auto& [s, p] : entries_) out.push_back(p);
    return out;
}

const FlowPersona* pick_side_speaker(const std::vector<FlowPersona>& speakers, const Side& side,
                                     const std::map<std::string, int>& speak_counts) {
    const FlowPersona* best = nullptr;
    long long best_count = std::numeric_limits<long long>::max();
    for (const auto& p : speakers) {
        if (persona_stance(p) != side) continue;
        const auto it = speak_counts.find(p.id);
        const long long c = it != speak_counts.end() ? it->second : 0;
        if (c < best_count) {
            best = &p;
            best_count = c;
        }
    }
    return best;
}

const FlowPersona* pick_chain_speaker(const std::vector<FlowPersona>& speakers, const Side& side, const std::vector<ArgumentNode>& path,
                                      const std::map<std::string, int>& speak_counts) {
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if (it->side != side) continue;
        for (const auto& p : speakers)
            if (p.id == it->personaId && persona_stance(p) == side) return &p;
    }
    return pick_side_speaker(speakers, side, speak_counts);
}

std::string render_exchange_closing(const ExchangeEnd& end, const GroundTally& pro, const GroundTally& con) {
    const std::string head =
        ended_by_turn_cap(end)
            ? "応答ターンの上限 (" + std::to_string(end.answered) + ") に達したので、応答待ち " + std::to_string(end.cutPending) +
                  " 件は決着しないまま (未決) まとめに入ります。"
            : std::string("論拠のやり取りが落ち着きました。");
    const std::string errored =
        end.errored > 0 ? "応答できなかった " + std::to_string(end.errored) + " 件も未決として扱います。" : std::string();
    const std::string drilled = end.drilled > 0 ? "形の足りない反論 " + std::to_string(end.drilled) + " 件を深掘りし、" +
                                                      std::to_string(end.drillRecovered) + " 件が反論として成り立ちました。"
                                                : std::string();
    return head + errored + drilled + "定立の論拠は、賛成側 " + std::to_string(pro.total) + " 件 (" + render_ground_tally(pro) + ")、反対側 " +
           std::to_string(con.total) + " 件 (" + render_ground_tally(con) + ") です。ここまでをまとめます。";
}

namespace {

/** 反論先ノードをゲート用の Position に見立てる (定立の論拠は元の Position、反論はその 1 論拠だけの主張)。 */
struct GateTarget {
    PositionRecord position;
    std::string groundId;
};

std::optional<GateTarget> gate_target(const ArgumentNode& node, const PositionsBySide& by_side) {
    const PositionRecord* owner = by_side.get(node.side);
    if (!owner) return std::nullopt;
    if (node.depth == 0 && node.groundId && !node.groundId->empty()) return GateTarget{*owner, *node.groundId};
    GateTarget t{*owner, node.id};
    t.position.claim = node.text;
    t.position.qualifier = owner->qualifier;
    Ground g;
    g.id = node.id;
    g.text = node.data;
    g.state = "unchallenged";
    if (node.warrant && !node.warrant->empty()) g.warrant = node.warrant;
    if (node.kind) g.kind = node.kind;
    t.position.grounds = {g};
    return t;
}

bool has_text(const std::optional<std::string>& s) { return s && !s->empty(); }

} // namespace

ExchangeEnd run_ground_exchange(const GroundExchangeArgs& args) {
    if (!args.exchange || !args.positionBySide || !args.utteranceIndex || !args.speakCounts || !args.turnLlm || !args.ruleCheckLlm ||
        !args.makeRecord || !args.commit || !args.applyGraph)
        throw std::invalid_argument("run_ground_exchange: required argument missing");
    GroundExchange& exchange = *args.exchange;
    const PositionsBySide& by_side = *args.positionBySide;
    const int round = args.round;
    const WarnFn log = args.log ? args.log : WarnFn([](const std::string&) {});
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string&) {});
    const std::string r = std::to_string(round);
    int turn = args.turn;
    int answered = 0;
    ExchangeEnd end;

    auto make = [&](const FlowPersona& persona, int t, const std::string& text) {
        RecordArgs a;
        a.persona = persona;
        a.round = round;
        a.turn = t;
        a.text = text;
        return a;
    };
    // 問いとして記録する (応答待ちは閉じない。深掘りの前の応答に使う)。
    auto record_question = [&](const FlowPersona& persona, int t, const std::string& text, const ArgumentNode& node) {
        RecordArgs a = make(persona, t, text);
        a.act = "question";
        a.targetId = node.utteranceId;
        const auto record = args.makeRecord(a);
        args.commit(record);
        args.applyGraph(record, "question", node.utteranceId);
    };
    auto commit_question = [&](const FlowPersona& persona, int t, const std::string& text, const ArgumentNode& node) {
        record_question(persona, t, text, node);
        exchange.fail(node.id);
    };

    for (auto node_opt = exchange.next(); node_opt; node_opt = exchange.next()) {
        const ArgumentNode node = *node_opt;
        if (answered >= args.turnCap) {
            const int cut = exchange.cut_remaining();
            end.reason = "turn_cap";
            end.cutPending = cut;
            log("論点 " + r + ": 応答ターン上限 " + std::to_string(args.turnCap) + " — 応答待ち " + std::to_string(cut) +
                " 件を打ち切り (未決として扱う)");
            break;
        }
        answered++;
        const Side side = opposing_side(node.side);
        const FlowPersona* picked = pick_chain_speaker(args.speakers, side, exchange.path(node.id), *args.speakCounts);
        const FlowPersona persona = picked ? *picked : args.debaters.of(side);
        turn += 1;
        const std::string ts = std::to_string(turn);

        // 反論への再反論は進行役が指名する (定立の論拠への反論は投げ合いの続きなので指名しない)。
        if (node.depth > 0) {
            RenderNominationArgs na;
            na.decision.kind = "respond";
            na.decision.personaId = persona.id;
            na.decision.targetUtteranceId = node.utteranceId;
            na.decision.allowedActs = {"rebut", "concede"};
            na.decision.reason = "反論への応答";
            na.personaName = persona.name;
            const auto it = args.utteranceIndex->find(node.utteranceId);
            if (it != args.utteranceIndex->end()) na.target = it->second;
            const std::string nomination = render_nomination_text(na);
            args.commit(args.makeRecord(make(args.facilitator, turn, nomination)));
        }

        // 応答 1 回ぶん (LLM 呼び出し → パース → 代理の帰属検査 → 前提ルールのゲート)。記録はしない。
        struct Attempt {
            bool llmError = false;
            std::string error;
            bool empty = false;
            ParsedExchangeTurn parsed;
            std::optional<proxy::AttributionResult> attribution;
            std::optional<RebutGateResult> gate;
        };
        auto attempt = [&](const std::optional<std::string>& drill_block) {
            Attempt a;
            ExchangePromptArgs pa;
            pa.persona = persona;
            pa.stance = persona_stance(persona);
            pa.issue = args.issue;
            for (const auto* p : by_side.values()) pa.positions.push_back(*p);
            pa.personaNames = args.personaNames;
            pa.path = exchange.path(node.id);
            pa.voicesBlock = args.voicesBlock;
            pa.drillDown = drill_block;
            persona_engine::LLMInvokeArgs ia;
            ia.system = args.paperSystem;
            ia.prompt = build_exchange_prompt(pa);
            ia.model = persona.model;
            const auto turn_llm = args.turnLlm(persona, turn);
            const auto result = turn_llm->invoke(ia);
            if (!result.ok) {
                a.llmError = true;
                a.error = result.error;
                return a;
            }
            a.parsed = parse_exchange_response(result.text, warn);
            if (a.parsed.text.empty()) {
                a.empty = true;
                return a;
            }
            if (a.parsed.act != "rebut") return a;
            // 代理議論: 本人の発言と申告した反論は、束の根拠 id と原文で裏付けられていなければ論拠にしない。
            if (persona.proxy) {
                proxy::CheckAttributionArgs ca;
                ca.claimKind = a.parsed.claimKind;
                ca.evidenceIds = a.parsed.evidenceIds;
                ca.data = a.parsed.data.value_or("");
                ca.scope.evidenceIds = persona.proxy->evidenceIds;
                for (const auto& [id, text] : persona.proxy->evidenceTexts) ca.scope.evidenceTexts.emplace(id, text);
                a.attribution = proxy::check_attribution(ca);
                if (!a.attribution->ok) return a;
            }
            // 前提ルールのゲート (dialectic.md §2.5): 反論は論拠 1 件 (データ + 論拠) で、突く要素を宣言する。
            if (auto target = gate_target(node, by_side)) {
                GateRebutArgs ga;
                ga.claim.attack = a.parsed.attack;
                ga.claim.groundId = target->groundId;
                ga.claim.data = a.parsed.data;
                ga.claim.warrant = a.parsed.warrant;
                ga.issue = args.issue;
                ga.position = target->position;
                // 立場の取り違え (自分の陣営と逆向きの反論) を判定するため、応答者の陣営の主張を渡す。
                if (const PositionRecord* own = by_side.get(side)) ga.ownPosition = *own;
                ga.rebutText = a.parsed.text;
                ga.judge = args.ruleCheck;
                ga.model = args.judgeModel;
                ga.warn = warn;
                const auto gate_llm = args.ruleCheckLlm(turn);
                a.gate = gate_rebut(*gate_llm, ga);
            }
            return a;
        };

        // 深掘り (spec/plan/2026-11-23-dialectic-drilldown.md): 形の崩れた応答には欠けている要素を名指しして
        // 同じ人に 1 回だけ言い直させる。最初の応答は問いとして残し、応答ターンの上限は消費しない。
        std::optional<std::string> drill_block;
        std::optional<RuleFoul> first_foul;
        bool drilled = false;
        for (;;) {
            const Attempt a = attempt(drill_block);
            if (a.llmError || a.empty) {
                if (drilled) {
                    // 言い直しに失敗: 最初の応答 (問いとして記録済み) で閉じる。相手の論拠は成り立ったまま。
                    warn("論点 " + r + " ターン " + ts + " (" + persona.name + "): 深掘りの言い直しに失敗 — 最初の応答を問いとして扱う");
                    exchange.fail(node.id);
                    if (first_foul) args.commit(args.makeRecord(make(args.facilitator, ++turn, render_foul_notice(persona.name, *first_foul))));
                    break;
                }
                if (a.llmError) {
                    warn("論点 " + r + " ターン " + ts + " (" + persona.name + ") LLM エラー: " + a.error + " — 未決として扱う");
                    RecordArgs ra = make(persona, turn, "[エラー: " + a.error + "]");
                    ra.isError = true;
                    args.commit(args.makeRecord(ra));
                } else {
                    warn("論点 " + r + " ターン " + ts + " (" + persona.name + "): 空応答 — 未決として扱う");
                }
                exchange.error(node.id);
                end.errored++;
                break;
            }
            const ParsedExchangeTurn& parsed = a.parsed;

            // 「認める」の申告が中身と合っているかを確かめる (追撃を「認める」に数えない)。
            // 中身が反論なら、論拠 (データ・論拠) を伴わない問いとして扱い相手の論拠は成り立ったままにする。
            if (parsed.act == "concede") {
                if (args.ruleCheck) {
                    const auto judge_llm = args.ruleCheckLlm(turn);
                    const std::string verdict = judge_concession(*judge_llm, node.text, parsed.text, args.judgeModel, warn);
                    if (verdict == "rebut") {
                        warn("論点 " + r + " ターン " + ts + " (" + persona.name + "): 「認める」の申告だが中身は反論 — question として扱う");
                        commit_question(persona, turn, parsed.text, node);
                        break;
                    }
                }
                RecordArgs ra = make(persona, turn, parsed.text);
                ra.act = "concede";
                ra.targetId = node.utteranceId;
                const auto record = args.makeRecord(ra);
                args.commit(record);
                args.applyGraph(record, "concede", node.utteranceId);
                exchange.accept(node.id);
                break;
            }

            if (a.attribution && !a.attribution->ok) {
                warn("論点 " + r + " ターン " + ts + " (" + persona.name + "): 帰属の検査に通らない (" + a.attribution->foul.value_or("") +
                     ") — question に格下げ");
                commit_question(persona, turn, parsed.text, node);
                args.commit(args.makeRecord(make(args.facilitator, ++turn, render_foul_notice(persona.name, a.attribution->foul.value_or("")))));
                break;
            }

            if (a.gate && a.gate->accepted && has_text(parsed.data)) {
                // 代理の反論は露出文に帰属の印 ([E2] / （推測）) を付け、区分と根拠 id を発話に残す。
                RecordArgs ra = make(persona, turn, parsed.text);
                ra.act = "rebut";
                ra.targetId = node.utteranceId;
                if (a.attribution && a.attribution->ok) {
                    ra.text = parsed.text + proxy::attribution_suffix(a.attribution->claimKind, a.attribution->evidenceIds);
                    ra.claimKind = a.attribution->claimKind;
                    ra.evidenceIds = a.attribution->evidenceIds;
                }
                const auto record = args.makeRecord(ra);
                args.commit(record);
                args.applyGraph(record, "rebut", node.utteranceId);
                NewNode nn;
                nn.utteranceId = record.id;
                nn.side = side;
                nn.personaId = persona.id;
                nn.data = *parsed.data;
                if (has_text(parsed.warrant)) nn.warrant = parsed.warrant;
                if (parsed.kind) nn.kind = parsed.kind;
                nn.text = parsed.text;
                exchange.rebut(node.id, nn);
                if (drilled) {
                    end.drillRecovered++;
                    log("論点 " + r + " ターン " + ts + " (" + persona.name + "): 深掘りの言い直しで反論の形が整った");
                }
                break;
            }

            const std::vector<std::string> missing = missing_rebut_elements(parsed, a.gate);
            if (args.drillDown && !drilled && !missing.empty()) {
                drilled = true;
                end.drilled++;
                if (a.gate && !a.gate->accepted) first_foul = a.gate->foul;
                log("論点 " + r + " ターン " + ts + " (" + persona.name + "): 反論の形が足りない — 深掘りして 1 回だけ言い直させる");
                record_question(persona, turn, parsed.text, node);
                args.commit(args.makeRecord(make(args.facilitator, ++turn, render_drilldown_request(persona.name, missing))));
                ++turn;
                drill_block = drilldown_prompt_block(missing, parsed.text);
                continue;
            }

            // 有効な反論にならなかった: 発言は問いとして残し、相手の論拠は成り立ったまま。
            commit_question(persona, turn, parsed.text, node);
            if (a.gate && !a.gate->accepted) {
                warn("論点 " + r + " ターン " + ts + " (" + persona.name + "): 前提ルール違反 (" + a.gate->foul + ") — question に格下げ");
                args.commit(args.makeRecord(make(args.facilitator, ++turn, render_foul_notice(persona.name, a.gate->foul))));
            }
            break;
        }
    }

    write_back_ground_states(exchange, by_side);

    // 論拠が落ち着いた (または上限で打ち切った) ら進行役がまとめに入る。まとめ文は定型 (言い換えで集計を誤らせない)。
    end.answered = answered;
    const GroundTally pro = exchange.tally("pro");
    const GroundTally con = exchange.tally("con");
    args.commit(args.makeRecord(make(args.facilitator, ++turn, render_exchange_closing(end, pro, con))));
    log("論点 " + r + ": 論拠の応酬 " + std::to_string(answered) + " ターンで終了 (" + end.reason + " / 賛成 " + render_ground_tally(pro) +
        " / 反対 " + render_ground_tally(con) + ")");
    end.turn = turn;
    return end;
}

void write_back_ground_states(const GroundExchange& exchange, const PositionsBySide& position_by_side) {
    const auto states = exchange.ground_states();
    for (PositionRecord* position : position_by_side.values()) {
        bool touched = false;
        for (auto& g : position->grounds) {
            // Map#get と同じく同じ id は後の値を使う。
            const GroundState* next = nullptr;
            for (const auto& [id, state] : states)
                if (id == g.id) next = &state;
            if (next && g.state != *next) {
                g.state = *next;
                touched = true;
            }
        }
        if (touched) update_position_grounds(position->id, position->grounds);
    }
}

} // namespace di::flow::dialectic
