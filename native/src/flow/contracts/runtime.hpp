#pragma once
// src/flow/contracts/runtime.ts の移植: Augur `contract-wrap` runtime。
// 包んだ処理の結果と例外はそのまま通し、判定 (verdict) だけを VESTIGIUM_LOGS_DIR (無ければ <cwd>/logs) の
// contracts.jsonl に weaver JSONL で追記する。引数と結果は記録しない (資格情報を含みうる)。
// 日次の切替: 有効ファイルはその日の分だけ。新しい日の最初の書き込みで前日分を archive/contracts-YYYY-MM-DD.jsonl へ移し、
// 保持期間 (AUGUR_CONTRACT_LOG_RETENTION_DAYS、既定 7 日) より古い archive を消す。日の境界は JST (UTC+9)。
// 観測は包んだ処理を決して変えない (記録の失敗は握りつぶす)。
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace di::flow::contracts {

/** 判定: nullopt = 成立 (true / undefined)、文字列 = 違反の理由。 */
using ContractVerdict = std::optional<std::string>;

struct ContractSpec {
    std::string contractId;
    std::string id;
    std::string where;
};

using Context = std::vector<std::pair<std::string, std::string>>;

/** JST の日付ラベル (YYYY-MM-DD)。 */
std::string day_key(std::int64_t epoch_ms);
/** AUGUR_CONTRACT_LOG_RETENTION_DAYS (0 以上の整数でなければ 7)。 */
int retention_days();
/** 1 行を記録する (失敗は握りつぶす)。now_ms は時刻 (テスト用に注入可)。 */
void record(const std::string& message, const Context& context, std::optional<std::int64_t> now_ms = std::nullopt);
/** テスト用: プロセス内の「確認済みの日」を忘れる。 */
void reset_for_test();

namespace detail {
Context base_context(const ContractSpec& spec);
Context with(Context ctx, const char* phase, const std::string& reason);
} // namespace detail

/**
 * contract(fn, {pre, post, ...}) の同期版。pre / post の判定を記録し、fn の結果をそのまま返す (例外も素通し)。
 * pre / post が例外を投げたら "contract predicate threw" を記録する。
 */
template <class R>
R run_contract(const ContractSpec& spec, const std::function<ContractVerdict()>& pre,
               const std::function<ContractVerdict(const R&)>& post, const std::function<R()>& fn) {
    const Context ctx = detail::base_context(spec);
    std::optional<std::string> pre_reason;
    try {
        if (pre) pre_reason = pre();
    } catch (...) {
        record("contract predicate threw", detail::with(ctx, "pre", "pre predicate threw"));
    }
    if (pre_reason) record("contract violated", detail::with(ctx, "pre", *pre_reason));
    R result = fn();
    try {
        const ContractVerdict post_reason = post ? post(result) : std::nullopt;
        if (post_reason) record("contract violated", detail::with(ctx, "post", *post_reason));
        else if (!pre_reason) {
            Context ok = ctx;
            ok.emplace_back("phase", "ok");
            record("contract observed", ok);
        }
    } catch (...) {
        record("contract predicate threw", detail::with(ctx, "post", "post predicate threw"));
    }
    return result;
}

} // namespace di::flow::contracts
