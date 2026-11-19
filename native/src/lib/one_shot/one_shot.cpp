#include "lib/one_shot/one_shot.hpp"

#include "base/env.hpp"
#include "lapilli/one_shot.hpp"

namespace di::lib::one_shot {

Launch prepare(const std::string& file, const std::vector<std::string>& args,
               const std::optional<std::map<std::string, std::string>>& env_in, const std::string& cwd) {
    namespace shared = lapilli::one_shot;
    const auto& source = env_in ? *env_in : env::snapshot();
    shared::PrepareOptions options;
    options.policy = shared::Policy::allow_node_entry;
    options.env.emplace(source.begin(), source.end());
    options.lookup = shared::system_lookup(*options.env);
    const std::string working_dir = cwd.empty() ? env::cwd() : cwd;
    options.lookup->cwd = working_dir;
    auto launch = shared::prepare(file, args, options);
    return {std::move(launch.file), std::move(launch.args), {launch.env.begin(), launch.env.end()}, working_dir};
}

proc::Result run(const std::string& file, const std::vector<std::string>& args, const RunOptions& options) {
    const Launch launch = prepare(file, args, options.env, options.cwd);
    proc::Options p;
    p.argv.push_back(launch.file);
    for (const auto& a : launch.args) p.argv.push_back(a);
    p.env = launch.env;
    p.cwd = launch.cwd;
    p.stdin_data = options.stdin_data;
    p.timeout = options.timeout;
    p.max_stdout = options.max_stdout;
    p.max_stderr = options.max_stderr;
    p.should_cancel = options.should_cancel;
    return proc::run(p);
}

} // namespace di::lib::one_shot
