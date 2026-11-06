#pragma once
// Anatomia CLI アクセス (I/O 境界。src/flow/anatomia/client.ts の移植)。
// `node <binPath> domains list --json` を引数配列で起動してドメインカードを取得する (シェルを経由しない)。
// カードが無ければ (autoDraft) `domains draft` で下地を作ってから再取得する。
// node の場所は env DISCUTERE_NODE (既定 "node")。TS の process.execPath に当たる。
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flow/anatomia/types.hpp"

namespace di::flow::anatomia {

struct CliResult {
    std::string stdout_text;
    int exitCode = 0;
};

/**
 * CLI 実行ランナー (テストで差し替えるための DI 境界)。
 * binPath を第 1 引数に、残りを CLI サブコマンド引数として実行する。cwd は Anatomia home。
 * 起動できなければ std::runtime_error。
 */
using AnatomiaCliRunner =
    std::function<CliResult(const std::string& bin_path, const std::vector<std::string>& args, std::chrono::milliseconds timeout,
                            const std::string& cwd)>;

/** 標準出力の上限 (TS の maxBuffer と同じ 32MiB)。 */
inline constexpr std::size_t kAnatomiaMaxStdout = 32u << 20;

/** 既定ランナー: di::proc::run (引数配列)。期限切れ・出力上限超過・シグナル終了は exit 1 (TS の execFile と同じ)。 */
CliResult default_anatomia_cli_runner(const std::string& bin_path, const std::vector<std::string>& args,
                                      std::chrono::milliseconds timeout, const std::string& cwd);

struct FetchAnatomiaDomainsOpts {
    /** Anatomia bin/anatomia.mjs のパス。空なら sibling リポ (`<cwd>/../Anatomia/bin/anatomia.mjs`)。 */
    std::string binPath;
    bool autoDraft = true;
    std::chrono::milliseconds timeout{180'000};
    AnatomiaCliRunner runner;
    std::function<void(const std::string&)> warn;
};

/** CLI の JSON 出力を読む (上限つき)。不正は std::runtime_error。 */
AnatomiaDomainsResponse parse_domains_json(const std::string& stdout_text);

/**
 * Anatomia からドメインカードを取得する。
 * fail-fast: bin が無い / CLI が非 0 終了 / JSON 不正は std::runtime_error (黙って空配列に逃げない)。
 * autoDraft で下地を作っても空なら空配列を返す。
 */
std::vector<AnatomiaDomainCard> fetch_anatomia_domains(const AnatomiaSource& source, const FetchAnatomiaDomainsOpts& opts = {});

} // namespace di::flow::anatomia
