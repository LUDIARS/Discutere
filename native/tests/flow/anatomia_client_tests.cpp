// flow/anatomia/client (tests/flow/anatomia-client.test.ts の移植 + 既定ランナーの子プロセス境界)。
#include "check.hpp"

#include <filesystem>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "flow/anatomia/client.hpp"

using namespace di;
using namespace di::flow::anatomia;

namespace {

const std::string kResponse = json_stringify(Json::parse(R"({
  "dir": "X/spec/domains",
  "domains": [
    {"name": "Trial", "description": "裁判", "mechanics": ["trial", 3], "specRefs": ["spec/A"], "rationale": "境界"},
    {"name": "NoName placeholder", "description": "x", "mechanics": []},
    {"description": "name 無しは落ちる", "mechanics": []},
    "not an object"
  ]
})"));

std::string temp_dir() {
    const auto dir = std::filesystem::temp_directory_path() / ("w1f-anatomia-" + std::to_string(env::pid()));
    std::filesystem::create_directories(dir / "root" / "bin");
    return fs::from_path(dir);
}

/** 存在する bin (中身は使わない)。 */
std::string existing_bin() {
    const std::string bin = temp_dir() + "/root/bin/anatomia.mjs";
    fs::write_file_atomic(bin, "// fake\n");
    return bin;
}

struct Recorder {
    std::vector<std::vector<std::string>> calls;
    std::vector<std::string> cwds;
    std::vector<std::string> list_outputs;
    std::size_t list_index = 0;
    AnatomiaCliRunner runner() {
        return [this](const std::string&, const std::vector<std::string>& args, std::chrono::milliseconds, const std::string& cwd) {
            calls.push_back(args);
            cwds.push_back(cwd);
            if (args[1] == "list") {
                const auto out = list_outputs[std::min(list_index, list_outputs.size() - 1)];
                ++list_index;
                return CliResult{out, 0};
            }
            return CliResult{"", 0};
        };
    }
};

} // namespace

TEST_CASE("list is parsed (cards without name dropped) and args / cwd are built like TS") {
    Recorder r;
    r.list_outputs = {kResponse};
    FetchAnatomiaDomainsOpts opts;
    opts.binPath = existing_bin();
    opts.runner = r.runner();
    const auto domains = fetch_anatomia_domains(AnatomiaSource::project("pagus"), opts);
    CHECK_EQ(domains.size(), 2u);
    CHECK_EQ(domains[0].name, "Trial");
    CHECK(domains[0].mechanics == std::vector<std::string>{"trial"});
    CHECK(domains[0].specRefs == std::vector<std::string>{"spec/A"});
    CHECK_EQ(json_stringify(to_json(domains[1])), R"({"name":"NoName placeholder","description":"x","mechanics":[],"specRefs":[]})");
    CHECK(r.calls[0] == (std::vector<std::string>{"domains", "list", "--project", "pagus", "--json"}));
    CHECK_EQ(r.cwds[0], fs::dirname(fs::dirname(opts.binPath)));
}

TEST_CASE("repoPath source becomes --repo") {
    Recorder r;
    r.list_outputs = {kResponse};
    FetchAnatomiaDomainsOpts opts;
    opts.binPath = existing_bin();
    opts.runner = r.runner();
    fetch_anatomia_domains(AnatomiaSource::repo_path("E:/x/Pagus"), opts);
    CHECK(r.calls[0] == (std::vector<std::string>{"domains", "list", "--repo", "E:/x/Pagus", "--json"}));
    CHECK_THROWS(fetch_anatomia_domains(AnatomiaSource::repo_path("  "), opts));
    CHECK_THROWS(fetch_anatomia_domains(AnatomiaSource::project(""), opts));
}

TEST_CASE("empty -> autoDraft runs draft then lists again; autoDraft=false stays empty") {
    const std::string empty = R"({"dir":"d","domains":[]})";
    Recorder r;
    r.list_outputs = {empty, kResponse};
    FetchAnatomiaDomainsOpts opts;
    opts.binPath = existing_bin();
    opts.runner = r.runner();
    std::vector<std::string> warnings;
    opts.warn = [&](const std::string& m) { warnings.push_back(m); };
    CHECK_EQ(fetch_anatomia_domains(AnatomiaSource::project("p"), opts).size(), 2u);
    CHECK_EQ(r.calls.size(), 3u);
    CHECK_EQ(r.calls[1][1], "draft");
    CHECK_EQ(warnings.size(), 1u);

    Recorder r2;
    r2.list_outputs = {empty};
    opts.runner = r2.runner();
    opts.autoDraft = false;
    CHECK_EQ(fetch_anatomia_domains(AnatomiaSource::project("p"), opts).size(), 0u);
    CHECK_EQ(r2.calls.size(), 1u);
}

TEST_CASE("fail fast: exit!=0 / broken JSON / missing domains / missing bin") {
    FetchAnatomiaDomainsOpts opts;
    opts.binPath = existing_bin();
    auto expect_error = [&](const std::string& needle) {
        try {
            fetch_anatomia_domains(AnatomiaSource::project("p"), opts);
        } catch (const std::exception& e) {
            if (!str::contains(e.what(), needle)) throw test::Failure(std::string("unexpected: ") + e.what());
            return;
        }
        throw test::Failure("no error for " + needle);
    };
    opts.runner = [](auto&&...) { return CliResult{"", 2}; };
    expect_error("list 失敗 (exit 2)");
    opts.runner = [](auto&&...) { return CliResult{"not json", 0}; };
    expect_error("JSON ではありません");
    opts.runner = [](auto&&...) { return CliResult{R"({"dir":"d"})", 0}; };
    expect_error("domains 配列がありません");
    opts.runner = [](auto&&...) { return CliResult{str::repeat("[", 200) + str::repeat("]", 200), 0}; };
    expect_error("JSON ではありません");
    opts.runner = [](auto&&...) { return CliResult{kResponse, 0}; };
    opts.binPath = temp_dir() + "/no/such/anatomia.mjs";
    expect_error("CLI が見つかりません");
}

#ifndef _WIN32
TEST_CASE("default runner: argv array via DISCUTERE_NODE, exit code, timeout and missing executable") {
    // DISCUTERE_NODE を /bin/sh にして、bin 引数の位置に置いたスクリプトを実行させる (シェル経由ではなく argv 配列)。
    const std::string dir = temp_dir();
    const std::string script = dir + "/root/bin/fake.sh";
    fs::write_file_atomic(script,
                          "if [ \"$2\" = list ]; then printf '%s' '{\"dir\":\"d\",\"domains\":[{\"name\":\"N\",\"description\":\"$1\"}]}'; "
                          "elif [ \"$2\" = slow ]; then sleep 5; else exit 3; fi\n");
    env::set_for_test("DISCUTERE_NODE", "/bin/sh");
    auto r = default_anatomia_cli_runner(script, {"domains", "list", "--project", "x; rm -rf /"}, std::chrono::milliseconds(10'000), dir);
    CHECK_EQ(r.exitCode, 0);
    CHECK_EQ(parse_domains_json(r.stdout_text).domains[0].name, "N");
    CHECK_EQ(default_anatomia_cli_runner(script, {"domains", "draft"}, std::chrono::milliseconds(10'000), dir).exitCode, 3);
    CHECK_EQ(default_anatomia_cli_runner(script, {"domains", "slow"}, std::chrono::milliseconds(200), dir).exitCode, 1);
    env::set_for_test("DISCUTERE_NODE", "/no/such/node-binary");
    CHECK_THROWS(default_anatomia_cli_runner(script, {"domains", "list"}, std::chrono::milliseconds(1000), dir));
    env::set_for_test("DISCUTERE_NODE", std::nullopt);
}
#endif

DI_TEST_MAIN()
