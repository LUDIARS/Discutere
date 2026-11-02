#pragma once
// Anatomia 連携の型定義 (議論前のゲーム情報供給。src/flow/anatomia/types.ts の移植)。
// Anatomia の `domains list --json` 出力 (ドメインカード) を取得し、ペーパーの事前情報 (メカニクス) に精製する。
// 取得は client、精製は refine (別担当)。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::flow::anatomia {

/** `domains list --json` が返すドメインカード 1 件 (本連携が使うフィールドのみ)。 */
struct AnatomiaDomainCard {
    std::string name;
    std::string description;
    std::vector<std::string> mechanics;
    std::vector<std::string> specRefs;
    std::optional<std::string> rationale;
    bool operator==(const AnatomiaDomainCard&) const = default;
};
Json to_json(const AnatomiaDomainCard& card);

/** `domains list --json` のレスポンス全体。 */
struct AnatomiaDomainsResponse {
    std::string dir;
    std::vector<AnatomiaDomainCard> domains;
};

/** 解析対象: 登録済みプロジェクト名 (`project`) か、リポジトリ絶対パス (`repoPath`) のどちらか一方。 */
struct AnatomiaSource {
    enum class Kind { project, repoPath } kind = Kind::project;
    std::string value;
    static AnatomiaSource project(std::string name) { return {Kind::project, std::move(name)}; }
    static AnatomiaSource repo_path(std::string path) { return {Kind::repoPath, std::move(path)}; }
};

} // namespace di::flow::anatomia
