#include "visualize/dump.hpp"

#include <stdexcept>

#include "base/fsutil.hpp"

namespace di::visualize {

namespace {
/** 既存 md の読み込み上限 (これを超える既存ファイルは「違う内容」として上書きする)。 */
constexpr std::size_t kMaxExistingMdBytes = 64u << 20;
} // namespace

std::string resolve_file_path(const std::string& type, const std::string& id, const DumpOptions& options) {
    const std::string root = options.rootDir ? *options.rootDir : fs::resolve("data/discussions");
    const auto dir = lookup(TYPE_TO_DIR, type);
    if (!dir) throw std::invalid_argument("unknown node type: " + type);
    // workspace / id は path.join で連結される。root の外に出る値は拒否する (移植で追加)。
    const auto path = fs::safe_join(root, options.workspaceId + "/" + *dir + "/" + id + ".md");
    if (!path)
        throw std::invalid_argument("dump path escapes root: " + options.workspaceId + "/" + id);
    return *path;
}

DumpResult dump_node(core::Core& core, const std::string& type, const std::string& id, const DumpOptions& options) {
    ExportResult result = export_node(core, type, options.workspaceId, id);
    const std::string file_path = resolve_file_path(type, id, options);
    fs::ensure_dir(fs::dirname(file_path));

    std::string written;
    if (fs::exists(file_path)) {
        const auto current = fs::read_file(file_path, kMaxExistingMdBytes);
        if (current && *current == result.markdown) {
            written = "unchanged";
        } else {
            if (!fs::write_file_atomic(file_path, result.markdown)) throw std::runtime_error("md を書き込めない: " + file_path);
            written = "updated";
        }
    } else {
        if (!fs::write_file_atomic(file_path, result.markdown)) throw std::runtime_error("md を書き込めない: " + file_path);
        written = "created";
    }
    DumpResult out;
    static_cast<ExportResult&>(out) = std::move(result);
    out.filePath = file_path;
    out.written = written;
    return out;
}

} // namespace di::visualize
