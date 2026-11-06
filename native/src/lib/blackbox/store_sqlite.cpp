#include "lib/blackbox/store_sqlite.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/time.hpp"
#include "lib/blackbox/fingerprint.hpp"

namespace di::lib::blackbox {

namespace {

const char* const CREATE_RULES = R"(
CREATE TABLE IF NOT EXISTS blackbox_rules (
  id TEXT PRIMARY KEY,
  domain TEXT NOT NULL,
  description TEXT NOT NULL,
  when_json TEXT NOT NULL,
  output_json TEXT NOT NULL,
  confidence REAL NOT NULL DEFAULT 0.7,
  state TEXT NOT NULL DEFAULT 'candidate',
  source TEXT NOT NULL DEFAULT 'manual',
  approvals INTEGER NOT NULL DEFAULT 0,
  rejections INTEGER NOT NULL DEFAULT 0,
  shadow_agreements INTEGER NOT NULL DEFAULT 0,
  shadow_conflicts INTEGER NOT NULL DEFAULT 0,
  proposals INTEGER NOT NULL DEFAULT 1,
  fingerprint TEXT NOT NULL DEFAULT '',
  priority INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);)";

const char* const CREATE_DECISIONS = R"(
CREATE TABLE IF NOT EXISTS blackbox_decisions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  domain TEXT NOT NULL,
  input_json TEXT,
  features_json TEXT,
  output_json TEXT,
  source TEXT NOT NULL,
  rule_id TEXT,
  confidence REAL NOT NULL,
  rationale TEXT NOT NULL,
  status TEXT NOT NULL,
  verdict TEXT,
  shadow_json TEXT,
  created_at TEXT NOT NULL,
  reviewed_at TEXT
);)";

/** DB の JSON 列を読む (JSON.parse と同じく壊れていれば投げる)。 */
Json parse_db_json(const std::string& text, const char* column) {
    std::string error;
    auto parsed = parse_json(text, db_json_limits(), &error);
    if (!parsed) throw std::runtime_error(std::string("blackbox: invalid JSON in ") + column + ": " + error);
    return std::move(*parsed);
}

Rule row_to_rule(const sql::Row& r) {
    Rule rule;
    rule.id = r.str("id");
    rule.domain = r.str("domain");
    rule.description = r.str("description");
    rule.when = parse_db_json(r.str("when_json"), "when_json");
    const std::string out = r.str("output_json");
    rule.output = out.empty() ? Json(nullptr) : parse_db_json(out, "output_json");
    rule.confidence = r.num("confidence");
    rule.state = r.str("state");
    rule.source = r.str("source");
    rule.approvals = r.i64("approvals");
    rule.rejections = r.i64("rejections");
    rule.shadowAgreements = r.i64("shadow_agreements");
    rule.shadowConflicts = r.i64("shadow_conflicts");
    rule.proposals = r.i64("proposals");
    rule.fingerprint = r.str("fingerprint");
    rule.priority = r.num("priority");
    rule.createdAt = r.str("created_at");
    rule.updatedAt = r.str("updated_at");
    return rule;
}

DecisionRecord row_to_decision(const sql::Row& r) {
    DecisionRecord d;
    d.id = r.i64("id");
    d.domain = r.str("domain");
    const std::string input = r.str("input_json");
    d.input = input.empty() ? Json(nullptr) : parse_db_json(input, "input_json");
    const std::string features = r.str("features_json");
    d.features = features.empty() ? Json::object() : parse_db_json(features, "features_json");
    const std::string output = r.str("output_json");
    d.output = output.empty() ? Json(nullptr) : parse_db_json(output, "output_json");
    d.source = r.str("source");
    d.ruleId = r.opt_str("rule_id");
    d.confidence = r.num("confidence");
    d.rationale = r.str("rationale");
    d.status = r.str("status");
    d.verdict = r.opt_str("verdict");
    const std::string shadow = r.str("shadow_json");
    if (!shadow.empty()) d.shadow = shadow_from_json(parse_db_json(shadow, "shadow_json"));
    d.createdAt = r.str("created_at");
    d.reviewedAt = r.opt_str("reviewed_at");
    return d;
}

} // namespace

JsonLimits db_json_limits() {
    JsonLimits l;
    l.max_bytes = 1u << 20;
    l.max_elements = 100'000;
    l.max_depth = 64;
    return l;
}

void ensure_blackbox_schema(sql::Database& db) {
    db.exec(CREATE_RULES);
    db.exec(CREATE_DECISIONS);

    const auto rule_cols = db.table_columns("blackbox_rules");
    if (std::find(rule_cols.begin(), rule_cols.end(), "state") == rule_cols.end()) {
        db.exec("ALTER TABLE blackbox_rules ADD COLUMN state TEXT NOT NULL DEFAULT 'candidate'");
        db.exec("ALTER TABLE blackbox_rules ADD COLUMN shadow_agreements INTEGER NOT NULL DEFAULT 0");
        db.exec("ALTER TABLE blackbox_rules ADD COLUMN shadow_conflicts INTEGER NOT NULL DEFAULT 0");
        db.exec("ALTER TABLE blackbox_rules ADD COLUMN proposals INTEGER NOT NULL DEFAULT 1");
        db.exec("ALTER TABLE blackbox_rules ADD COLUMN fingerprint TEXT NOT NULL DEFAULT ''");
        // 旧 enabled/approvals/rejections から state を再構成する。
        db.exec(R"(
      UPDATE blackbox_rules SET state = CASE
        WHEN enabled = 1 AND approvals >= 3 THEN 'auto'
        WHEN enabled = 1 THEN 'trial'
        WHEN rejections >= 3 THEN 'retired'
        ELSE 'candidate'
      END)");
    }
    // fingerprint 未計算の行を埋める (新規作成直後は 0 行なので no-op)。
    const auto missing = db.all("SELECT id, when_json, output_json FROM blackbox_rules WHERE fingerprint = ''");
    for (const auto& row : missing) {
        const std::string out = row.str("output_json");
        const std::string fp = rule_fingerprint(parse_db_json(row.str("when_json"), "when_json"),
                                                out.empty() ? Json(nullptr) : parse_db_json(out, "output_json"));
        db.run("UPDATE blackbox_rules SET fingerprint = ? WHERE id = ?", {fp, row.str("id")});
    }

    const auto dec_cols = db.table_columns("blackbox_decisions");
    if (std::find(dec_cols.begin(), dec_cols.end(), "shadow_json") == dec_cols.end()) {
        db.exec("ALTER TABLE blackbox_decisions ADD COLUMN shadow_json TEXT");
    }

    db.exec("CREATE INDEX IF NOT EXISTS idx_blackbox_rules_domain ON blackbox_rules(domain)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_blackbox_decisions_domain ON blackbox_decisions(domain, created_at)");
}

// ── RuleStore ──────────────────────────────────────────────────────────────

SqliteRuleStore::SqliteRuleStore(sql::DatabasePtr db, std::function<std::string()> now)
    : db_(std::move(db)), now_(now ? std::move(now) : std::function<std::string()>([] { return time::now_iso(); })) {}

std::vector<Rule> SqliteRuleStore::list_by_domain(const std::string& domain) {
    std::vector<Rule> out;
    for (const auto& row : db_->all("SELECT * FROM blackbox_rules WHERE domain = ? ORDER BY priority DESC, created_at ASC", {domain}))
        out.push_back(row_to_rule(row));
    return out;
}

std::optional<Rule> SqliteRuleStore::find_by_fingerprint(const std::string& domain, const std::string& fingerprint) {
    auto row = db_->get("SELECT * FROM blackbox_rules WHERE domain = ? AND fingerprint = ? LIMIT 1", {domain, fingerprint});
    if (!row) return std::nullopt;
    return row_to_rule(*row);
}

std::optional<Rule> SqliteRuleStore::get(const std::string& id) {
    auto row = db_->get("SELECT * FROM blackbox_rules WHERE id = ?", {id});
    if (!row) return std::nullopt;
    return row_to_rule(*row);
}

Rule SqliteRuleStore::insert(const RuleDraft& draft) {
    const std::string ts = now_();
    const std::string id = crypto::uuid_v4();
    db_->run(
        R"(INSERT INTO blackbox_rules
         (id, domain, description, when_json, output_json, confidence, state, source,
          approvals, rejections, shadow_agreements, shadow_conflicts, proposals, fingerprint,
          priority, created_at, updated_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0, 0, 1, ?, ?, ?, ?))",
        {id, draft.domain, draft.description, json_stringify(draft.when), json_stringify(draft.output),
         draft.confidence.value_or(0.7), draft.state.value_or("candidate"), draft.source.value_or("manual"),
         rule_fingerprint(draft.when, draft.output), draft.priority.value_or(0), ts, ts});
    auto rule = get(id);
    if (!rule) throw std::runtime_error("blackbox: inserted rule not found");
    return *rule;
}

std::optional<Rule> SqliteRuleStore::update(const std::string& id, const RulePatch& patch) {
    auto cur = get(id);
    if (!cur) return std::nullopt;
    Rule next = *cur;
    apply_patch(next, patch);
    db_->run(
        R"(UPDATE blackbox_rules SET
         state = ?, approvals = ?, rejections = ?,
         shadow_agreements = ?, shadow_conflicts = ?, proposals = ?,
         confidence = ?, priority = ?, description = ?, updated_at = ?
       WHERE id = ?)",
        {next.state, next.approvals, next.rejections, next.shadowAgreements, next.shadowConflicts, next.proposals,
         next.confidence, next.priority, next.description, now_(), id});
    return get(id);
}

// ── DecisionLedger ─────────────────────────────────────────────────────────

SqliteDecisionLedger::SqliteDecisionLedger(sql::DatabasePtr db) : db_(std::move(db)) {}

std::int64_t SqliteDecisionLedger::record(const DecisionInput& rec) {
    Json shadow = Json::array();
    for (const auto& e : rec.shadow) shadow.push_back(to_json(e));
    const auto info = db_->run(
        R"(INSERT INTO blackbox_decisions
         (domain, input_json, features_json, output_json, source, rule_id,
          confidence, rationale, status, verdict, shadow_json, created_at, reviewed_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, ?, ?, NULL))",
        {rec.domain, json_stringify(rec.input), json_stringify(rec.features.is_null() ? Json::object() : rec.features),
         json_stringify(rec.output), rec.source, rec.ruleId, rec.confidence, rec.rationale, rec.status,
         json_stringify(shadow), rec.createdAt});
    return info.last_insert_rowid;
}

std::optional<DecisionRecord> SqliteDecisionLedger::get(std::int64_t id) {
    auto row = db_->get("SELECT * FROM blackbox_decisions WHERE id = ?", {id});
    if (!row) return std::nullopt;
    return row_to_decision(*row);
}

void SqliteDecisionLedger::set_verdict(std::int64_t id, const std::string& verdict, const std::string& reviewed_at) {
    db_->run("UPDATE blackbox_decisions SET verdict = ?, reviewed_at = ? WHERE id = ?", {verdict, reviewed_at, id});
}

std::vector<DecisionRecord> SqliteDecisionLedger::list_pending(const std::optional<std::string>& domain, std::int64_t limit) {
    std::vector<sql::Row> rows =
        domain && !domain->empty()
            ? db_->all(R"(SELECT * FROM blackbox_decisions WHERE status='pending_review' AND verdict IS NULL AND domain=?
           ORDER BY created_at DESC LIMIT ?)",
                       {*domain, limit})
            : db_->all(R"(SELECT * FROM blackbox_decisions WHERE status='pending_review' AND verdict IS NULL
           ORDER BY created_at DESC LIMIT ?)",
                       {limit});
    std::vector<DecisionRecord> out;
    for (const auto& r : rows) out.push_back(row_to_decision(r));
    return out;
}

std::vector<DecisionRecord> SqliteDecisionLedger::list_recent(const std::string& domain, std::int64_t limit) {
    std::vector<DecisionRecord> out;
    for (const auto& r : db_->all("SELECT * FROM blackbox_decisions WHERE domain = ? ORDER BY id DESC LIMIT ?", {domain, limit}))
        out.push_back(row_to_decision(r));
    return out;
}

} // namespace di::lib::blackbox
