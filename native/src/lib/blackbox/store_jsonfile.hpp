#pragma once
// JSON ファイル実装の RuleStore / DecisionLedger (store-jsonfile.ts)。
// ルールと判断 ledger を 1 つの JSON ファイルに write-through 永続化する。
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "lib/blackbox/blackbox.hpp"

namespace di::lib::blackbox {

struct FileState {
    std::vector<Rule> rules;
    std::vector<DecisionRecord> decisions;
    std::int64_t seq = 0;
};

struct JsonFileStoreOptions {
    /** ledger に保持する直近判断数 (肥大防止)。既定 1000。ルールは丸めない。 */
    std::optional<std::int64_t> maxDecisions;
    /** 時刻源 (テスト用)。 */
    std::function<std::string()> now;
};

/** 読むファイルの上限。超えるファイルは読まずに std::runtime_error (上書きで消さないため)。 */
inline constexpr std::size_t kBlackboxFileMaxBytes = 64u << 20;

/** rules / decisions を 1 つの JSON に共載する共有ファイル。 */
class BlackboxJsonFile {
public:
    explicit BlackboxJsonFile(std::string path);
    /** 無い・壊れているファイルは空状態。 */
    FileState load() const;
    void save(const FileState& state) const;
    /** load → fn → save を 1 つの排他区間で行う (同じファイルを触る store 同士の取りこぼしを防ぐ)。 */
    std::recursive_mutex& mutex() const { return mutex_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    mutable std::recursive_mutex mutex_;
};

class JsonFileRuleStore : public RuleStore {
public:
    explicit JsonFileRuleStore(std::shared_ptr<BlackboxJsonFile> file, const JsonFileStoreOptions& opts = {});
    std::vector<Rule> list_by_domain(const std::string& domain) override;
    std::optional<Rule> find_by_fingerprint(const std::string& domain, const std::string& fingerprint) override;
    std::optional<Rule> get(const std::string& id) override;
    Rule insert(const RuleDraft& draft) override;
    std::optional<Rule> update(const std::string& id, const RulePatch& patch) override;

private:
    std::shared_ptr<BlackboxJsonFile> file_;
    std::function<std::string()> now_;
};

class JsonFileDecisionLedger : public DecisionLedger {
public:
    explicit JsonFileDecisionLedger(std::shared_ptr<BlackboxJsonFile> file, const JsonFileStoreOptions& opts = {});
    std::int64_t record(const DecisionInput& rec) override;
    std::optional<DecisionRecord> get(std::int64_t id) override;
    void set_verdict(std::int64_t id, const std::string& verdict, const std::string& reviewed_at) override;
    std::vector<DecisionRecord> list_pending(const std::optional<std::string>& domain = std::nullopt,
                                             std::int64_t limit = 50) override;
    std::vector<DecisionRecord> list_recent(const std::string& domain, std::int64_t limit) override;

private:
    std::shared_ptr<BlackboxJsonFile> file_;
    std::int64_t max_decisions_;
};

/** ファイルに書く判断レコードの形 ({ ...rec, id, verdict, reviewedAt } の順)。 */
Json decision_record_to_file_json(const DecisionRecord& r);

/** JSON ファイルで束ねた blackbox を 1 つ作る (CLI / ゲームサーバ向け)。 */
BlackBox make_file_blackbox(const std::string& path, const EngineOptions& engine_opts = {},
                            const JsonFileStoreOptions& store_opts = {});

} // namespace di::lib::blackbox
