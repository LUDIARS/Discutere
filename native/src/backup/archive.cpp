#include "backup/archive.hpp"

#include "base/sidecar.hpp"

namespace di::backup {

Json to_json(const CreateArchiveResult& r) {
    Json o = Json::object();
    o["outFile"] = r.outFile;
    o["included"] = r.included;
    o["skipped"] = r.skipped;
    return o;
}

CreateArchiveResult create_archive(const CreateArchiveArgs& args) {
    Json input = Json::object();
    input["targets"] = args.targets;
    input["outFile"] = args.outFile;
    if (args.cwd) input["cwd"] = *args.cwd;
    sidecar::Options options;
    options.timeout = kArchiveTimeout;
    options.max_output = 1u << 20;
    options.json_limits = JsonLimits{1u << 20, 10'000, 8};
    const auto r = sidecar::run("backup-archive.ts", input, options);
    if (!r.ok) throw std::runtime_error(r.error);
    CreateArchiveResult out;
    out.outFile = json_string(r.result, "outFile", args.outFile);
    for (const auto& s : json_array(r.result, "included"))
        if (s.is_string()) out.included.push_back(s.get<std::string>());
    for (const auto& s : json_array(r.result, "skipped"))
        if (s.is_string()) out.skipped.push_back(s.get<std::string>());
    return out;
}

} // namespace di::backup
