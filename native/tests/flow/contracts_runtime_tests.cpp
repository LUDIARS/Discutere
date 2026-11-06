// src/flow/contracts/runtime.ts / spec-zip-entry.contract.ts の移植の確認。
#include "check.hpp"

#include <chrono>
#include <filesystem>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "flow/contracts/runtime.hpp"
#include "flow/contracts/spec_zip_entry_contract.hpp"

using namespace di;
using namespace di::flow::contracts;

namespace {

struct LogsDir {
    std::string path;
    LogsDir() {
        path = fs::from_path(std::filesystem::temp_directory_path() / ("w1e-contracts-" + crypto::uuid_v4()));
        env::set_for_test("VESTIGIUM_LOGS_DIR", path);
        env::set_for_test("AUGUR_CONTRACT_LOG_RETENTION_DAYS", std::nullopt);
        reset_for_test();
    }
    ~LogsDir() {
        std::error_code ec;
        std::filesystem::remove_all(fs::to_path(path), ec);
        env::set_for_test("VESTIGIUM_LOGS_DIR", std::nullopt);
    }
    std::vector<Json> lines() const {
        std::vector<Json> out;
        auto text = fs::read_file(fs::join(path, "contracts.jsonl"));
        if (!text) return out;
        for (const auto& l : str::split(*text, "\n"))
            if (!l.empty()) out.push_back(parse_json_or_throw(l));
        return out;
    }
};

void set_mtime(const std::string& path, std::int64_t ms) {
    const auto sys = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
    std::filesystem::last_write_time(fs::to_path(path), std::chrono::clock_cast<std::filesystem::file_time_type::clock>(sys));
}

} // namespace

TEST_CASE("day key is JST") {
    const auto t = time::from_civil_utc(2026, 10, 13, 14, 59, 59);
    CHECK_EQ(day_key(t), std::string("2026-10-13"));
    CHECK_EQ(day_key(t + 1000), std::string("2026-10-14"));  // 15:00 UTC = 翌日 0:00 JST
}

TEST_CASE("retention days env") {
    env::set_for_test("AUGUR_CONTRACT_LOG_RETENTION_DAYS", "3");
    CHECK_EQ(retention_days(), 3);
    env::set_for_test("AUGUR_CONTRACT_LOG_RETENTION_DAYS", "1.5");
    CHECK_EQ(retention_days(), 7);
    env::set_for_test("AUGUR_CONTRACT_LOG_RETENTION_DAYS", "-1");
    CHECK_EQ(retention_days(), 7);
    env::set_for_test("AUGUR_CONTRACT_LOG_RETENTION_DAYS", " ");
    CHECK_EQ(retention_days(), 7);
    env::set_for_test("AUGUR_CONTRACT_LOG_RETENTION_DAYS", "0");
    CHECK_EQ(retention_days(), 0);
    env::set_for_test("AUGUR_CONTRACT_LOG_RETENTION_DAYS", std::nullopt);
}

TEST_CASE("run_contract records verdicts without touching results") {
    LogsDir logs;
    const ContractSpec spec{"C-1", "87417249", "src/flow/spec-source.ts:189"};
    std::function<ContractVerdict()> pre_ok = [] { return ContractVerdict{}; };
    std::function<ContractVerdict(const int&)> post_bad = [](const int& r) {
        return r > 5 ? ContractVerdict("too big") : ContractVerdict{};
    };
    CHECK_EQ(run_contract<int>(spec, pre_ok, post_bad, [] { return 3; }), 3);
    CHECK_EQ(run_contract<int>(spec, pre_ok, post_bad, [] { return 9; }), 9);
    std::function<ContractVerdict()> pre_throw = []() -> ContractVerdict { throw std::runtime_error("x"); };
    CHECK_EQ(run_contract<int>(spec, pre_throw, {}, [] { return 1; }), 1);
    std::function<ContractVerdict()> pre_bad = [] { return ContractVerdict("bad args"); };
    CHECK_EQ(run_contract<int>(spec, pre_bad, {}, [] { return 2; }), 2);
    CHECK_THROWS(run_contract<int>(spec, pre_ok, post_bad, []() -> int { throw std::runtime_error("inner"); }));
    const auto lines = logs.lines();
    CHECK_EQ(lines.size(), std::size_t{5});
    CHECK_EQ(lines[0]["msg"].get<std::string>(), std::string("contract observed"));
    CHECK_EQ(json_stringify(lines[0]["ctx"]["phase"]), std::string("\"ok\""));
    CHECK_EQ(lines[1]["msg"].get<std::string>(), std::string("contract violated"));
    CHECK_EQ(lines[1]["ctx"]["reason"].get<std::string>(), std::string("too big"));
    // ctx のキー順は TS と同じ (contract, id, where, observed_at, phase, reason)
    std::vector<std::string> keys;
    for (auto it = lines[1]["ctx"].begin(); it != lines[1]["ctx"].end(); ++it) keys.push_back(it.key());
    CHECK_EQ(keys, (std::vector<std::string>{"contract", "id", "where", "observed_at", "phase", "reason"}));
    CHECK_EQ(lines[2]["msg"].get<std::string>(), std::string("contract predicate threw"));
    CHECK_EQ(lines[2]["ctx"]["phase"].get<std::string>(), std::string("pre"));
    // pre の判定が例外でも、結果の観測は続ける (TS と同じ)
    CHECK_EQ(lines[3]["ctx"]["phase"].get<std::string>(), std::string("ok"));
    // pre 違反のときは ok を記録しない (post も違反なし)
    CHECK_EQ(lines[4]["ctx"]["reason"].get<std::string>(), std::string("bad args"));
}

TEST_CASE("daily rotation archives yesterday and prunes old archives") {
    LogsDir logs;
    const std::int64_t now = time::from_civil_utc(2026, 10, 20, 3, 0, 0);
    const std::string active = fs::join(logs.path, "contracts.jsonl");
    fs::ensure_dir(logs.path);
    CHECK(fs::write_file_atomic(active, "{\"old\":1}\n"));
    set_mtime(active, now - 24LL * 3600 * 1000);
    const std::string archive = fs::join(logs.path, "archive");
    fs::ensure_dir(archive);
    CHECK(fs::write_file_atomic(fs::join(archive, "contracts-2026-10-01.jsonl"), "x\n"));
    CHECK(fs::write_file_atomic(fs::join(archive, ".contracts-2026-10-02.1.2.part"), "x\n"));
    CHECK(fs::write_file_atomic(fs::join(archive, "contracts-2026-10-18.jsonl"), "keep\n"));
    CHECK(fs::write_file_atomic(fs::join(archive, "other.txt"), "keep\n"));
    record("contract observed", {{"phase", "ok"}}, now);
    const auto yesterday = fs::read_file(fs::join(archive, "contracts-2026-10-19.jsonl"));
    CHECK(yesterday.has_value());
    CHECK_EQ(*yesterday, std::string("{\"old\":1}\n"));
    CHECK(!fs::exists(fs::join(archive, "contracts-2026-10-01.jsonl")));
    CHECK(!fs::exists(fs::join(archive, ".contracts-2026-10-02.1.2.part")));
    CHECK(fs::exists(fs::join(archive, "contracts-2026-10-18.jsonl")));
    CHECK(fs::exists(fs::join(archive, "other.txt")));
    CHECK_EQ(logs.lines().size(), std::size_t{1});
    // 既に同じ日の archive がある → 追記する
    reset_for_test();
    CHECK(fs::write_file_atomic(active, "{\"more\":1}\n"));
    set_mtime(active, now - 24LL * 3600 * 1000);
    record("again", {}, now);
    CHECK_EQ(*fs::read_file(fs::join(archive, "contracts-2026-10-19.jsonl")), std::string("{\"old\":1}\n{\"more\":1}\n"));
}

TEST_CASE("record never throws on an unwritable directory") {
    env::set_for_test("VESTIGIUM_LOGS_DIR", "/proc/definitely/not/writable");
    record("x", {});
    env::set_for_test("VESTIGIUM_LOGS_DIR", std::nullopt);
}

TEST_CASE("spec zip entry contract predicates") {
    CHECK(!spec_zip_entry_post(10, 10).has_value());
    CHECK(spec_zip_entry_post(11, 10).has_value());
    CHECK(spec_zip_entry_post(64u * 1024 * 1024 + 1, 1u << 30).has_value());
    CHECK(!spec_zip_entry_post(64u * 1024 * 1024, 1u << 30).has_value());
    CHECK(!spec_zip_entry_post_throw("ファイル a.pptx: ZIP extraction failed", "ファイル a.pptx").has_value());
    CHECK(spec_zip_entry_post_throw("ZIP extraction failed", "ファイル a.pptx").has_value());
}

DI_TEST_MAIN()
