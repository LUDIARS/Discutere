#include "ludus/economy_analyzer.hpp"

#include <algorithm>
#include <mutex>
#include <set>

#include "base/crypto.hpp"
#include "base/executor.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/kg_registry.hpp"
#include "db/repository.hpp"

namespace di::ludus {

Json to_json(const EconomyGraph& g) {
    Json loops = Json::array();
    for (const auto& l : g.economyLoops) loops.push_back(l);
    return Json{{"gameTitle", g.gameTitle}, {"nodes", g.nodes},       {"edges", g.edges},           {"summary", g.summary},
                {"economyLoops", loops},     {"analyzedAt", g.analyzedAt}, {"mechanicCount", g.mechanicCount}};
}

std::string to_slug(const std::string& game_title) {
    // JS の toLowerCase で ASCII の英数字になる非 ASCII は U+0130 (İ → i + U+0307) と U+212A (K → k) だけ。
    std::string out;
    bool pending_dash = false;
    std::size_t i = 0;
    while (i < game_title.size()) {
        const std::uint32_t cp = str::next_codepoint(game_title, i);
        char c = 0;
        if (cp < 0x80) {
            const char a = static_cast<char>(cp);
            if (a >= 'A' && a <= 'Z') c = static_cast<char>(a - 'A' + 'a');
            else if ((a >= 'a' && a <= 'z') || (a >= '0' && a <= '9')) c = a;
        } else if (cp == 0x0130) {
            c = 'i';  // 続く U+0307 は区切りになる
        } else if (cp == 0x212A) {
            c = 'k';
        }
        if (c) {
            if (pending_dash && !out.empty()) out += '-';
            pending_dash = cp == 0x0130;
            out += c;
        } else {
            pending_dash = true;
        }
    }
    // 先頭の区切りは out が空なので付かない。末尾の区切りは付けない (^-|-$ の除去と同じ)。
    return out;
}

std::string extract_json(const std::string& text) {
    // /```(?:json)?\s*([\s\S]*?)```/ — 最初の ``` から次の ``` まで。
    const auto open = text.find("```");
    if (open != std::string::npos) {
        std::size_t p = open + 3;
        const auto close_any = text.find("```", p);
        if (close_any != std::string::npos) {
            if (text.compare(p, 4, "json") == 0 && p + 4 <= close_any) p += 4;
            return str::trim(std::string_view(text).substr(p, close_any - p));
        }
    }
    const auto start = text.find('{');
    const auto end = text.rfind('}');
    if (start != std::string::npos && end != std::string::npos) {
        if (end < start) return std::string();  // JS の slice(start, end+1) は空
        return text.substr(start, end - start + 1);
    }
    return str::trim(text);
}

const std::string& economy_system_prompt() {
    static const std::string prompt = R"(You are a game design analyst specializing in internal economy and systems design.

Your task: analyze a game's mechanics and produce a structured graph that shows resource relationships and gameplay loops.

The user message includes information already gathered from two databases:
1. Guide mechanics — mechanics data extracted from game guides and articles
2. Discussion knowledge — player/designer discussion insights from the knowledge graph

Use this information to understand the game before producing the final graph. If it is empty, rely on your own knowledge of the game.

MECHANIC GRANULARITY — use these coarse categories for nodes (not fine-grained systems):
- engine: A core mechanism that drives the game's primary loop (e.g., "deckbuilding engine", "wave spawn engine", "income loop")
- attrition: A depletion/wear system where resources diminish over time or through play (e.g., "HP attrition", "deck thinning", "time pressure")
- win_condition: The goal state or victory/failure condition (e.g., "boss defeat", "score threshold", "survival time")
- resource: A tracked quantity that flows through the game (e.g., "gold", "cards-in-hand", "energy", "upgrade points")
- experience: A player emotional/cognitive state created by the system (e.g., "escalating tension", "build satisfaction", "decision anxiety")

EDGE TYPES:
- produces: X generates Y
- consumes: X uses up Y
- enables: X makes Y possible/easier
- requires: X cannot work without Y
- drives: X pushes Y into action
- suppresses: X reduces Y's effectiveness
- leads_to: X creates player experience Y
- reinforces: positive feedback — X creates more X (through Y)
- feedback: X's output returns as X's input

OUTPUT: A single JSON object (no markdown, no explanation):
{
  "nodes": [{ "id": "kebab-id", "type": "engine|attrition|win_condition|resource|experience", "label": "Short Name", "description": "1-2 sentences" }],
  "edges": [{ "id": "e1", "source": "node-id", "target": "node-id", "type": "produces|consumes|...", "label": "short verb" }],
  "summary": "2-3 sentences describing the core economy loop and primary gameplay experience.",
  "economyLoops": ["Description of each major feedback loop"]
}

Keep it focused: 6-15 nodes, 8-20 edges. Every node must be in at least one edge. Output only JSON.)";
    return prompt;
}

std::string build_user_message(const std::string& game_title, const std::string& guide_text, const std::string& discussion_text) {
    return "Analyze the internal economy and resource relationships of: **" + game_title + "**\n\n" +
           "Use the gathered mechanics data below, then produce the graph JSON.\n" +
           "Focus on: what resources exist, how the engine creates/consumes them, what leads to attrition, and what the win condition demands.\n\n" +
           "# Guide mechanics\n" + guide_text + "\n\n# Discussion knowledge\n" + discussion_text;
}

std::string query_guide_mechanics_text(sql::Database& main_db, const std::string& workspace_id, const std::string& game_title, int limit) {
    auto self = main_db.shared_from_this();
    const auto rows = db::LudusMechanicRepo(self).search_by_game(workspace_id, game_title);
    const std::size_t n = std::min<std::size_t>(rows.size(), static_cast<std::size_t>(std::clamp(std::min(limit, 50), 0, 50)));
    if (n == 0) {
        return "No mechanics found in guide data for \"" + game_title + "\". Try the exact game title or a variant spelling.";
    }
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = rows[i];
        lines.push_back("- " + m.mechanic + " (confidence: " + std::to_string(m.confidence) + "%, source: " +
                        (m.sourceTitle ? *m.sourceTitle : m.sourceUrl) + ")");
    }
    return "Guide mechanics for \"" + game_title + "\" (" + std::to_string(n) + " results):\n" + str::join(lines, "\n");
}

std::string search_discussion_knowledge_text(core::Core* core, const std::string& workspace_id, const std::string& keyword, int limit) {
    const int lim = std::clamp(std::min(limit, 20), 0, 20);
    if (!core) return "Discussion knowledge search unavailable (KG not initialized or empty).";
    try {
        const std::string like = "%" + keyword + "%";
        const auto rows = core->raw().all(
            R"SQL(SELECT u.normalized_content AS content, s.title AS session_title
           FROM utterances u
           JOIN sessions s ON s.id = u.session_id
           WHERE u.workspace_id = ?
             AND (u.normalized_content LIKE ? OR u.raw_content LIKE ?)
           ORDER BY u.created_at DESC
           LIMIT ?)SQL",
            {workspace_id, like, like, lim}, sql::QueryLimits{20, 8u << 20});
        if (rows.empty()) return "No discussion utterances found containing \"" + keyword + "\".";
        std::vector<std::string> lines;
        for (const auto& r : rows) {
            const auto title = r.opt_str("session_title");
            lines.push_back("[" + (title ? *title : std::string("?")) + "] " + str::js_slice(r.str("content"), 0, 200));
        }
        return "Discussion knowledge for \"" + keyword + "\" (" + std::to_string(rows.size()) + " results):\n" + str::join(lines, "\n");
    } catch (...) {
        return "Discussion knowledge search unavailable (KG not initialized or empty).";
    }
}

namespace {

sql::Database& main_db_of(const AnalyzeEconomyDeps& deps) { return db::Repository(deps.mainDb).db(); }

std::unique_ptr<core::Core> open_core(const AnalyzeEconomyDeps& deps) {
    try {
        if (deps.openCore) return deps.openCore();
        return core::create_core(core::CoreOptions{core::resolve_active_kg_path(config::get_config()), std::nullopt});
    } catch (...) {
        return nullptr;
    }
}

/** LLM の出力 JSON から graph を組む (配列・文字列でない値は捨てる)。 */
void fill_graph(EconomyGraph& g, const Json& parsed) {
    if (!parsed.is_object()) return;
    auto objects = [](const Json& v) {
        Json out = Json::array();
        if (v.is_array())
            for (const auto& e : v)
                if (e.is_object()) out.push_back(e);
        return out;
    };
    if (parsed.contains("nodes")) g.nodes = objects(parsed["nodes"]);
    if (parsed.contains("edges")) g.edges = objects(parsed["edges"]);
    if (parsed.contains("summary") && parsed["summary"].is_string()) g.summary = parsed["summary"].get<std::string>();
    if (parsed.contains("economyLoops") && parsed["economyLoops"].is_array())
        for (const auto& l : parsed["economyLoops"])
            if (l.is_string()) g.economyLoops.push_back(l.get<std::string>());
}

EconomyGraph graph_from_cache(const std::string& graph_json) {
    JsonLimits limits{kEconomyGraphJsonMaxBytes, 200'000, 32};
    const Json j = parse_json_or_throw(graph_json, limits);
    EconomyGraph g;
    if (j.is_object()) {
        g.gameTitle = json_string(j, "gameTitle");
        fill_graph(g, j);
        g.analyzedAt = json_string(j, "analyzedAt");
        g.mechanicCount = json_number(j, "mechanicCount");
    }
    return g;
}

std::mutex& inflight_mutex() {
    static std::mutex m;
    return m;
}
std::set<std::string>& inflight() {
    static std::set<std::string> s;
    return s;
}

} // namespace

AnalyzeEconomyResult analyze_economy(const std::string& workspace_id, const std::string& game_title, const AnalyzeEconomyDeps& deps,
                                     bool force) {
    AnalyzeEconomyResult out;
    log::info("[economy-analyzer] start: \"" + game_title + "\" ws=" + workspace_id);
    try {
        sql::Database& main_db = main_db_of(deps);
        db::EconomyGraphRepo graphs(main_db.shared_from_this());
        if (!force) {
            if (const auto cached = graphs.find_by_game(workspace_id, game_title)) {
                log::info("[economy-analyzer] cache hit: \"" + game_title + "\"");
                out.ok = true;
                out.cached = true;
                out.graph = graph_from_cache(cached->graphJson);
                return out;
            }
        }

        if (!deps.llm) {
            out.error = "LLM not configured";
            return out;
        }

        const auto mechanics = db::LudusMechanicRepo(main_db.shared_from_this()).search_by_game(workspace_id, game_title);
        log::info("[economy-analyzer] calling LLM for \"" + game_title + "\" (" + std::to_string(mechanics.size()) +
                  " guide mechanics available)");

        // ツール呼び出しの代わりに、同じ問い合わせを先に行ってプロンプトに載せる。
        const std::string guide_text = query_guide_mechanics_text(main_db, workspace_id, game_title, 30);
        std::string discussion_text;
        {
            auto core = open_core(deps);
            std::vector<std::string> parts{search_discussion_knowledge_text(core.get(), workspace_id, game_title, 10)};
            std::set<std::string> seen{game_title};
            int extra = 0;
            for (const auto& m : mechanics) {
                if (extra >= kEconomyExtraKeywordSearches) break;
                if (str::trim(m.mechanic).empty() || !seen.insert(m.mechanic).second) continue;
                parts.push_back(search_discussion_knowledge_text(core.get(), workspace_id, m.mechanic, 5));
                ++extra;
            }
            if (core) core->close();
            discussion_text = str::join(parts, "\n\n");
        }

        persona_engine::LLMInvokeArgs args;
        args.system = economy_system_prompt();
        args.prompt = build_user_message(game_title, guide_text, discussion_text);
        args.model = ANALYSIS_MODEL;
        args.maxTokens = ANALYSIS_MAX_TOKENS;
        args.timeoutMs = ANALYSIS_TIMEOUT_MS;
        args.conversationOnly = true;
        persona_engine::LLMResult res;
        try {
            res = deps.llm->invoke(args);
        } catch (const std::exception& e) {
            res = persona_engine::LLMResult::failure(std::string("llm call failed: ") + e.what());
        }
        if (res.ok && str::trim(res.text).empty()) res = persona_engine::LLMResult::failure("empty response");
        if (!res.ok) {
            log::error("[economy-analyzer] LLM error: " + res.error);
            out.error = res.error;
            return out;
        }
        if (res.text.size() > kEconomyMaxResponseBytes) {
            out.error = "JSON parse failed: response too large";
            log::error("[economy-analyzer] " + out.error);
            return out;
        }

        std::string perr;
        const auto parsed = parse_json(extract_json(res.text), JsonLimits{kEconomyMaxResponseBytes, 100'000, 32}, &perr);
        if (!parsed) {
            log::error("[economy-analyzer] JSON parse failed: " + perr + "\nraw:\n" + str::js_slice(res.text, 0, 500));
            out.error = "JSON parse failed: " + perr;
            return out;
        }

        EconomyGraph graph;
        graph.gameTitle = game_title;
        fill_graph(graph, *parsed);
        graph.analyzedAt = time::now_iso();
        graph.mechanicCount = static_cast<double>(mechanics.size());

        graphs.upsert(Json{{"id", crypto::uuid_v4()},
                           {"workspaceId", workspace_id},
                           {"gameTitle", game_title},
                           {"graphJson", json_stringify(to_json(graph))},
                           {"mechanicCount", static_cast<double>(mechanics.size())}});

        log::info("[economy-analyzer] done: \"" + game_title + "\" nodes=" + std::to_string(graph.nodes.size()) +
                  " edges=" + std::to_string(graph.edges.size()));
        out.ok = true;
        out.graph = std::move(graph);
        return out;
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = e.what();
        return out;
    }
}

bool analyze_economy_in_background(const std::string& workspace_id, const std::string& game_title, AnalyzeEconomyDeps deps, bool force) {
    const std::string key = workspace_id + "\n" + game_title;
    {
        std::lock_guard lock(inflight_mutex());
        if (!inflight().insert(key).second) return false;
    }
    const bool accepted = background_executor().submit([workspace_id, game_title, deps = std::move(deps), force, key] {
        const auto r = analyze_economy(workspace_id, game_title, deps, force);
        if (!r.ok) log::error("[economy-graph] analyze error: " + r.error);
        std::lock_guard lock(inflight_mutex());
        inflight().erase(key);
    });
    if (!accepted) {
        std::lock_guard lock(inflight_mutex());
        inflight().erase(key);
        log::warn("[economy-graph] analyze rejected: background queue is full");
    }
    return accepted;
}

} // namespace di::ludus
