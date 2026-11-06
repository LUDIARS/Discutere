#include "flow/anatomia/client.hpp"

#include <stdexcept>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/process.hpp"
#include "base/strings.hpp"

namespace di::flow::anatomia {
namespace {

/** sibling リポ既定パス (`<cwd>/../Anatomia/bin/anatomia.mjs`)。 */
std::string default_sibling_bin() {
    return fs::resolve(fs::join(fs::join(fs::join(fs::join(env::cwd(), ".."), "Anatomia"), "bin"), "anatomia.mjs"));
}

std::vector<std::string> source_args(const AnatomiaSource& source) {
    if (source.kind == AnatomiaSource::Kind::project) {
        if (str::trim(source.value).empty()) throw std::runtime_error("Anatomia source.project が空です");
        return {"--project", source.value};
    }
    if (str::trim(source.value).empty()) throw std::runtime_error("Anatomia source.repoPath が空です");
    return {"--repo", source.value};
}

std::vector<std::string> string_items(const Json& value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const auto& v : value)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

} // namespace

Json to_json(const AnatomiaDomainCard& card) {
    Json j = Json::object();
    j["name"] = card.name;
    j["description"] = card.description;
    j["mechanics"] = card.mechanics;
    j["specRefs"] = card.specRefs;
    if (card.rationale) j["rationale"] = *card.rationale;
    return j;
}

CliResult default_anatomia_cli_runner(const std::string& bin_path, const std::vector<std::string>& args,
                                      std::chrono::milliseconds timeout, const std::string& cwd) {
    proc::Options options;
    options.argv.push_back(env::get_nonempty("DISCUTERE_NODE").value_or("node"));
    options.argv.push_back(bin_path);
    options.argv.insert(options.argv.end(), args.begin(), args.end());
    options.cwd = cwd;
    options.timeout = timeout;
    options.max_stdout = kAnatomiaMaxStdout;
    options.max_stderr = 1u << 20;
    const auto result = proc::run(options);
    if (!result.started)
        throw std::runtime_error("Anatomia CLI を起動できません (" + options.argv[0] + "): " + result.start_error);
    // execFile は exit!=0 / timeout を err にする。timeout・maxBuffer 超過・シグナル終了は exit 1 として返す。
    if (result.timed_out || result.cancelled || result.output_exceeded || result.exit_code < 0) return {result.out, 1};
    return {result.out, result.exit_code};
}

AnatomiaDomainsResponse parse_domains_json(const std::string& stdout_text) {
    std::string error;
    auto parsed = parse_json(stdout_text, JsonLimits{kAnatomiaMaxStdout, 2'000'000, 64}, &error);
    if (!parsed) throw std::runtime_error("Anatomia domains 出力が JSON ではありません: " + error);
    const Json& obj = *parsed;
    if (!obj.is_object() || !obj.contains("domains") || !obj["domains"].is_array())
        throw std::runtime_error("Anatomia domains 出力に domains 配列がありません");
    AnatomiaDomainsResponse response;
    response.dir = json_string(obj, "dir");
    for (const auto& d : obj["domains"]) {
        if (!d.is_object()) continue;
        const std::string name = json_string(d, "name");
        if (name.empty()) continue;
        AnatomiaDomainCard card;
        card.name = name;
        card.description = json_string(d, "description");
        card.mechanics = string_items(d.value("mechanics", Json(nullptr)));
        card.specRefs = string_items(d.value("specRefs", Json(nullptr)));
        card.rationale = json_opt_string(d, "rationale");
        response.domains.push_back(std::move(card));
    }
    return response;
}

std::vector<AnatomiaDomainCard> fetch_anatomia_domains(const AnatomiaSource& source, const FetchAnatomiaDomainsOpts& opts) {
    const AnatomiaCliRunner runner = opts.runner ? opts.runner : AnatomiaCliRunner(default_anatomia_cli_runner);
    const std::string trimmed = str::trim(opts.binPath);
    const std::string bin_path = trimmed.empty() ? default_sibling_bin() : trimmed;
    if (!fs::exists(bin_path))
        throw std::runtime_error("Anatomia CLI が見つかりません: " + bin_path +
                                 " (flow.anatomia.binPath / DISCUTERE_ANATOMIA_BIN を設定してください)");

    const auto src = source_args(source);
    // Anatomia home はプロジェクト登録簿 (`<home>/.anatomia/projects.json`) の場所。binPath = <root>/bin/anatomia.mjs。
    const std::string anatomia_home = fs::dirname(fs::dirname(bin_path));

    auto with_src = [&](const char* verb) {
        std::vector<std::string> args{"domains", verb};
        args.insert(args.end(), src.begin(), src.end());
        args.push_back("--json");
        return args;
    };
    auto list = [&]() {
        const auto r = runner(bin_path, with_src("list"), opts.timeout, anatomia_home);
        if (r.exitCode != 0) throw std::runtime_error("anatomia domains list 失敗 (exit " + std::to_string(r.exitCode) + ")");
        return parse_domains_json(r.stdout_text).domains;
    };

    auto domains = list();
    if (domains.empty() && opts.autoDraft) {
        if (opts.warn) opts.warn("Anatomia: ドメイン下地が無いため domains draft を実行します (初回は解析で時間がかかります)");
        const auto r = runner(bin_path, with_src("draft"), opts.timeout, anatomia_home);
        if (r.exitCode != 0) throw std::runtime_error("anatomia domains draft 失敗 (exit " + std::to_string(r.exitCode) + ")");
        domains = list();
    }
    return domains;
}

} // namespace di::flow::anatomia
