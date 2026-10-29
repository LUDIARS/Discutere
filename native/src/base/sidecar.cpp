#include "base/sidecar.hpp"

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/process.hpp"
#include "base/strings.hpp"

namespace di::sidecar {

std::string repo_root() {
    if (auto r = env::get_nonempty("DISCUTERE_REPO_ROOT")) return fs::resolve(*r);
    return env::cwd();
}

Result run(std::string_view script, const Json& input, const Options& options) {
    Result out;
    const std::string root = repo_root();
    auto path = fs::safe_join(fs::join(root, "native/sidecar"), script);
    if (!path || !fs::is_file(*path)) {
        out.error = "sidecar script not found";
        return out;
    }
    proc::Options p;
    p.argv = {env::get_nonempty("DISCUTERE_NODE").value_or("node"), "--import", "tsx", *path};
    p.cwd = root;
    p.stdin_data = json_stringify(input);
    p.timeout = options.timeout;
    p.max_stdout = options.max_output;
    p.max_stderr = 256 * 1024;
    const auto r = proc::run(p);
    if (!r.started) {
        out.error = "sidecar start failed: " + r.start_error;
        return out;
    }
    if (r.timed_out) {
        out.error = "sidecar timeout";
        return out;
    }
    if (r.output_exceeded) {
        out.error = "sidecar output too large";
        return out;
    }
    std::string error;
    auto parsed = parse_json(str::trim(r.out), options.json_limits, &error);
    if (!parsed || !parsed->is_object()) {
        out.error = "sidecar returned invalid output (exit " + std::to_string(r.exit_code) + "): " + str::js_slice(r.err, 0, 300);
        return out;
    }
    if (!json_bool(*parsed, "ok")) {
        out.error = json_string(*parsed, "error", "sidecar failed");
        return out;
    }
    out.ok = true;
    out.result = (*parsed)["result"];
    return out;
}

} // namespace di::sidecar
