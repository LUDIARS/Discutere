#include "flow/dialectic/exchange_runner.hpp"

#include <limits>
#include <stdexcept>

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

int run_ground_exchange(const GroundExchangeArgs& args) {
    if (!args.exchange || !args.positionBySide || !args.utteranceIndex || !args.speakCounts || !args.facilitatorLlm || !args.turnLlm ||
        !args.ruleCheckLlm || !args.makeRecord || !args.commit || !args.applyGraph)
        throw std::invalid_argument("run_ground_exchange: required argument missing");
    GroundExchange& exchange = *args.exchange;
    const PositionsBySide& by_side = *args.positionBySide;
    const int round = args.round;
    const WarnFn log = args.log ? args.log : WarnFn([](const std::string&) {});
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string&) {});
    const std::string r = std::to_string(round);
    int turn = args.turn;
    int answered = 0;

    auto make = [&](const FlowPersona& persona, int t, const std::string& text) {
        RecordArgs a;
        a.persona = persona;
        a.round = round;
        a.turn = t;
        a.text = text;
        return a;
    };
    auto commit_question = [&](const FlowPersona& persona, int t, const std::string& text, const ArgumentNode& node) {
        RecordArgs a = make(persona, t, text);
        a.act = "question";
        a.targetId = node.utteranceId;
        const auto record = args.makeRecord(a);
        args.commit(record);
        args.applyGraph(record, "question", node.utteranceId);
        exchange.fail(node.id);
    };

    for (auto node_opt = exchange.next(); node_opt; node_opt = exchange.next()) {
        const ArgumentNode node = *node_opt;
        if (answered >= args.turnCap) {
            const int cut = exchange.cut_remaining();
            log("論点 " + r + ": 応答ターン上限 " + std::to_string(args.turnCap) + " — 応答待ち " + std::to_string(cut) +
                " 件を打ち切り (決着つかずとして扱う)");
            break;
        }
        answered++;
        const Side side = opposing_side(node.side);
        const FlowPersona* picked = pick_side_speaker(args.speakers, side, *args.speakCounts);
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
            na.model = args.facilitator.model;
            na.warn = warn;
            const std::string nomination = render_nomination(*args.facilitatorLlm, na);
            args.commit(args.makeRecord(make(args.facilitator, turn, nomination)));
        }

        ExchangePromptArgs pa;
        pa.persona = persona;
        pa.stance = persona_stance(persona);
        pa.issue = args.issue;
        for (const auto* p : by_side.values()) pa.positions.push_back(*p);
        pa.personaNames = args.personaNames;
        pa.path = exchange.path(node.id);
        pa.voicesBlock = args.voicesBlock;
        const std::string prompt = build_exchange_prompt(pa);
        persona_engine::LLMInvokeArgs ia;
        ia.system = args.paperSystem;
        ia.prompt = prompt;
        ia.model = persona.model;
        const auto turn_llm = args.turnLlm(persona, turn);
        const auto result = turn_llm->invoke(ia);
        if (!result.ok) {
            warn("論点 " + r + " ターン " + ts + " (" + persona.name + ") LLM エラー: " + result.error + " — 応答なしとして扱う");
            RecordArgs a = make(persona, turn, "[エラー: " + result.error + "]");
            a.isError = true;
            args.commit(args.makeRecord(a));
            exchange.fail(node.id);
            continue;
        }

        const ParsedExchangeTurn parsed = parse_exchange_response(result.text, warn);
        if (parsed.text.empty()) {
            log("論点 " + r + " ターン " + ts + " (" + persona.name + "): 空応答 — 応答なしとして扱う");
            exchange.fail(node.id);
            continue;
        }

        // 「認める」の申告が中身と合っているかを確かめる (追撃を「認める」に数えない)。
        // 中身が反論なら、論拠 (データ・論拠) を伴わない問いとして扱い相手の論拠は成り立ったままにする。
        if (parsed.act == "concede" && args.ruleCheck) {
            const auto judge_llm = args.ruleCheckLlm(turn);
            const std::string verdict = judge_concession(*judge_llm, node.text, parsed.text, args.judgeModel, warn);
            if (verdict == "rebut") {
                warn("論点 " + r + " ターン " + ts + " (" + persona.name + "): 「認める」の申告だが中身は反論 — question として扱う");
                commit_question(persona, turn, parsed.text, node);
                continue;
            }
        }

        if (parsed.act == "concede") {
            RecordArgs a = make(persona, turn, parsed.text);
            a.act = "concede";
            a.targetId = node.utteranceId;
            const auto record = args.makeRecord(a);
            args.commit(record);
            args.applyGraph(record, "concede", node.utteranceId);
            exchange.accept(node.id);
            continue;
        }

        // 代理議論: 本人の発言と申告した反論は、束の根拠 id と原文で裏付けられていなければ論拠にしない。
        std::optional<proxy::AttributionResult> attribution;
        if (persona.proxy && parsed.act == "rebut") {
            proxy::CheckAttributionArgs ca;
            ca.claimKind = parsed.claimKind;
            ca.evidenceIds = parsed.evidenceIds;
            ca.data = parsed.data.value_or("");
            ca.scope.evidenceIds = persona.proxy->evidenceIds;
            for (const auto& [id, text] : persona.proxy->evidenceTexts) ca.scope.evidenceTexts.emplace(id, text);
            attribution = proxy::check_attribution(ca);
        }
        if (attribution && !attribution->ok) {
            warn("論点 " + r + " ターン " + ts + " (" + persona.name + "): 帰属の検査に通らない (" + attribution->foul.value_or("") +
                 ") — question に格下げ");
            commit_question(persona, turn, parsed.text, node);
            args.commit(args.makeRecord(make(args.facilitator, ++turn, render_foul_notice(persona.name, attribution->foul.value_or("")))));
            continue;
        }

        // 前提ルールのゲート (dialectic.md §2.5): 反論は論拠 1 件 (データ + 論拠) で、突く要素を宣言する。
        std::optional<RebutGateResult> gate;
        if (parsed.act == "rebut") {
            if (auto target = gate_target(node, by_side)) {
                GateRebutArgs ga;
                ga.claim.attack = parsed.attack;
                ga.claim.groundId = target->groundId;
                ga.claim.data = parsed.data;
                ga.claim.warrant = parsed.warrant;
                ga.issue = args.issue;
                ga.position = target->position;
                // 立場の取り違え (自分の陣営と逆向きの反論) を判定するため、応答者の陣営の主張を渡す。
                if (const PositionRecord* own = by_side.get(side)) ga.ownPosition = *own;
                ga.rebutText = parsed.text;
                ga.judge = args.ruleCheck;
                ga.model = args.judgeModel;
                ga.warn = warn;
                const auto gate_llm = args.ruleCheckLlm(turn);
                gate = gate_rebut(*gate_llm, ga);
            }
        }

        if (gate && gate->accepted && has_text(parsed.data)) {
            // 代理の反論は露出文に帰属の印 ([E2] / （推測）) を付け、区分と根拠 id を発話に残す。
            RecordArgs a = make(persona, turn, parsed.text);
            a.act = "rebut";
            a.targetId = node.utteranceId;
            if (attribution && attribution->ok) {
                a.text = parsed.text + proxy::attribution_suffix(attribution->claimKind, attribution->evidenceIds);
                a.claimKind = attribution->claimKind;
                a.evidenceIds = attribution->evidenceIds;
            }
            const auto record = args.makeRecord(a);
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
            continue;
        }

        // 有効な反論にならなかった: 発言は問いとして残し、相手の論拠は成り立ったまま。
        commit_question(persona, turn, parsed.text, node);
        if (gate && !gate->accepted) {
            warn("論点 " + r + " ターン " + ts + " (" + persona.name + "): 前提ルール違反 (" + gate->foul + ") — question に格下げ");
            args.commit(args.makeRecord(make(args.facilitator, ++turn, render_foul_notice(persona.name, gate->foul))));
        }
    }

    write_back_ground_states(exchange, by_side);

    // 論拠が落ち着いたら進行役がまとめに入る。
    const auto pro = exchange.standing_count("pro");
    const auto con = exchange.standing_count("con");
    const std::string closing = render_facilitator_line(
        *args.facilitatorLlm,
        "論拠のやり取りが落ち着きました。崩れずに残った論拠は、賛成側 " + std::to_string(pro.standing) + "/" + std::to_string(pro.total) +
            " 件、" + "反対側 " + std::to_string(con.standing) + "/" + std::to_string(con.total) + " 件です。ここまでをまとめます。",
        args.facilitator.model, warn);
    args.commit(args.makeRecord(make(args.facilitator, ++turn, closing)));
    log("論点 " + r + ": 論拠の応酬 " + std::to_string(answered) + " ターンで終了 (賛成 " + std::to_string(pro.standing) + "/" +
        std::to_string(pro.total) + " / 反対 " + std::to_string(con.standing) + "/" + std::to_string(con.total) + ")");
    return turn;
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
